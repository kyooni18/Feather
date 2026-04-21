#ifndef FEATHER_FSSCHEDULER_H
#define FEATHER_FSSCHEDULER_H

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <type_traits>
#include <utility>
#include <vector>
#include <queue>
#include "FSTime.hpp"

// ---------------------------------------------------------------------------
// Budget bit-packing utilities
//
// Packs two independent 4-bit budget (priority) values into one byte:
//   high nibble (bits 7–4): first  budget value (0–15)
//   low  nibble (bits 3–0): second budget value (0–15)
//
// Kept for compact storage when two task budgets must fit in a single byte.
// Does NOT encode weight/quantum or any scheduling semantics.
// ---------------------------------------------------------------------------
static inline uint8_t fs_budget_pack(uint8_t a, uint8_t b) {
    return static_cast<uint8_t>(((a & 0x0F) << 4) | (b & 0x0F));
}
static inline uint8_t fs_budget_high(uint8_t packed) {
    return static_cast<uint8_t>((packed >> 4) & 0x0F);
}
static inline uint8_t fs_budget_low(uint8_t packed) {
    return static_cast<uint8_t>(packed & 0x0F);
}

enum FSSchedulerPeriodicTaskRepeatAllocationType {
    Relative,
    Absolute
};

// ---------------------------------------------------------------------------
// FSCallback – lightweight move-only callable wrapper
//
// Replaces std::function<void()> with a trampoline + heap-allocated context
// pointer. Any void() callable (lambda with captures, functor) can be
// constructed at the call site — no pre-declared static functions needed.
//
// Layout: 3 raw pointers = 24 bytes (vs std::function's typical 32–48 bytes).
// No virtual dispatch table; invocation is a direct function-pointer call.
// ---------------------------------------------------------------------------
struct FSCallback {
    void*  ctx       = nullptr;
    void (*invoke_f )(void*) = nullptr;
    void (*destroy_f)(void*) = nullptr;

    FSCallback() = default;

    // Constructs from any void() callable (lambda, functor, etc.).
    // The callable is heap-allocated once; no copies are ever made.
    template<typename F>
    explicit FSCallback(F&& f) {
        using Decay = std::decay_t<F>;
        auto* heap  = new Decay(std::forward<F>(f));
        ctx        = heap;
        invoke_f   = [](void* p) { (*static_cast<Decay*>(p))(); };
        destroy_f  = [](void* p) { delete static_cast<Decay*>(p); };
    }

    ~FSCallback() {
        if (destroy_f) destroy_f(ctx);
    }

    FSCallback(FSCallback&& o) noexcept
        : ctx(o.ctx), invoke_f(o.invoke_f), destroy_f(o.destroy_f) {
        o.ctx = nullptr; o.invoke_f = nullptr; o.destroy_f = nullptr;
    }

    FSCallback& operator=(FSCallback&& o) noexcept {
        if (this != &o) {
            if (destroy_f) destroy_f(ctx);
            ctx = o.ctx; invoke_f = o.invoke_f; destroy_f = o.destroy_f;
            o.ctx = nullptr; o.invoke_f = nullptr; o.destroy_f = nullptr;
        }
        return *this;
    }

    FSCallback(const FSCallback&)            = delete;
    FSCallback& operator=(const FSCallback&) = delete;

    void operator()() const { if (invoke_f) invoke_f(ctx); }
    explicit operator bool() const { return invoke_f != nullptr; }
};

// ---------------------------------------------------------------------------
// FSScheduler
// ---------------------------------------------------------------------------
class FSScheduler {
private:

    FSTime clock;

    static constexpr uint8_t MaxBudget = 0x0F;

    static uint8_t normalize_budget(uint8_t priority) {
        return static_cast<uint8_t>(priority & MaxBudget);
    }

    static uint8_t slices_for_budget(uint8_t budget) {
        return static_cast<uint8_t>((budget & MaxBudget) + 1u);
    }

    // -----------------------------------------------------------------------
    // Ready queue — one-shot work scheduled by events or other runtime hooks.
    // Higher budgets run first; matching budgets preserve FIFO order.
    // -----------------------------------------------------------------------
    struct ReadyTaskRecord {
        FSCallback task;
        uint8_t    budget   = 0;
        uint64_t   sequence = 0;
    };

    struct ReadyTaskCmp {
        bool operator()(const ReadyTaskRecord& a, const ReadyTaskRecord& b) const {
            if (a.budget != b.budget) {
                return a.budget < b.budget;
            }
            return a.sequence > b.sequence;
        }
    };

    std::priority_queue<ReadyTaskRecord,
                        std::vector<ReadyTaskRecord>,
                        ReadyTaskCmp> ready_task_queue;
    uint64_t next_ready_sequence = 0;

    // -----------------------------------------------------------------------
    // Instant tasks — stored once in a flat vector; executed by a simple
    // weighted round-robin cursor directly on the original records.
    // A budget of 0 still receives 1 execution slice; 15 receives 16 slices.
    // -----------------------------------------------------------------------
    struct InstantTaskRecord {
        FSCallback task;
        uint64_t   id     = 0;
        uint8_t    budget = 0;
    };

    std::vector<InstantTaskRecord> instant_task_records;
    size_t instant_rr_cursor = 0;
    uint8_t instant_rr_budget_remaining = 0;

    // -----------------------------------------------------------------------
    // Timed tasks — single min-heap keyed on next_fire_ms.
    //   period_ms == 0  →  one-shot deferred task
    //   period_ms  > 0  →  periodic task
    // -----------------------------------------------------------------------
    struct TimedTaskRecord {
        uint64_t   next_fire_ms = 0;
        uint64_t   id        = 0;
        FSCallback task;
        uint32_t   period_ms = 0;
        uint8_t    budget    = 0;
        FSSchedulerPeriodicTaskRepeatAllocationType repeat_type =
            FSSchedulerPeriodicTaskRepeatAllocationType::Absolute;
    };

    struct TimedTaskCmp {
        bool operator()(const TimedTaskRecord& a, const TimedTaskRecord& b) const {
            if (a.next_fire_ms != b.next_fire_ms) {
                return a.next_fire_ms > b.next_fire_ms;
            }
            if (a.budget != b.budget) {
                return a.budget < b.budget;
            }
            return a.id > b.id;
        }
    };

    struct TimedTaskState {
        bool enabled     = true;
        bool cancelled   = false;
        bool is_periodic = false;
    };

    std::priority_queue<TimedTaskRecord,
                        std::vector<TimedTaskRecord>,
                        TimedTaskCmp> timed_heap;
    std::unordered_map<uint64_t, TimedTaskState> timed_task_states;

    uint64_t next_wakeup_time = 0;
    uint64_t next_id          = 1;

    void maybe_shrink_timed_heap();
    void prune_cancelled_timed_tasks();

public:

    explicit FSScheduler(FSTime& clock_src);

    uint64_t now_ms() {
        return clock.now_ms();
    }

    // -----------------------------------------------------------------------
    // Template API
    //
    // Each method accepts any void() callable directly — lambdas with
    // local-variable captures, functors, etc. — and wraps it into an
    // FSCallback. The external name is "priority"; internally it is stored as
    // a 4-bit budget. Ready tasks use budget for ordering, instant tasks use it
    // for weighted round-robin slices, and timed tasks use it to break ties.
    // -----------------------------------------------------------------------

    template<typename F>
    void enqueue_ready_task(F&& task, uint8_t priority) {
        ready_task_queue.push(
            ReadyTaskRecord{
                FSCallback(std::forward<F>(task)),
                normalize_budget(priority),
                next_ready_sequence++
            }
        );
    }

    template<typename F>
    uint64_t add_instant_task(F&& task, uint8_t priority) {
        const uint64_t id = next_id++;
        instant_task_records.push_back(
            InstantTaskRecord{
                FSCallback(std::forward<F>(task)),
                id,
                normalize_budget(priority)
            }
        );
        return id;
    }

    template<typename F>
    uint64_t add_deferred_task(F&& task, uint64_t timestamp_ms, uint8_t priority) {
        const uint64_t id = next_id++;
        TimedTaskRecord rec{
            timestamp_ms,
            id,
            FSCallback(std::forward<F>(task)),
            0u,
            normalize_budget(priority),
            FSSchedulerPeriodicTaskRepeatAllocationType::Absolute
        };
        timed_heap.push(std::move(rec));
        timed_task_states.emplace(id, TimedTaskState{true, false, false});
        return id;
    }

    template<typename F>
    uint64_t add_periodic_task(
        F&&      task,
        uint64_t start_timestamp_ms,
        uint32_t period_ms,
        uint8_t  priority,
        FSSchedulerPeriodicTaskRepeatAllocationType allocation_type =
            FSSchedulerPeriodicTaskRepeatAllocationType::Absolute
    ) {
        const uint64_t id = next_id++;
        TimedTaskRecord rec{
            start_timestamp_ms,
            id,
            FSCallback(std::forward<F>(task)),
            period_ms,
            normalize_budget(priority),
            allocation_type
        };
        timed_heap.push(std::move(rec));
        timed_task_states.emplace(id, TimedTaskState{true, false, true});
        return id;
    }

    uint64_t calculate_next_wakeup_time_ms(uint64_t now_ms);
    uint64_t get_next_wakeup_time_ms() const;
    bool     has_ready_tasks() const;
    bool     cancel_task(uint64_t task_id);
    bool     set_task_enabled(uint64_t task_id, bool enabled);
    bool     is_task_enabled(uint64_t task_id) const;
    void     step();
};

#endif // FEATHER_FSSCHEDULER_H
