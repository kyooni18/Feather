#ifndef FEATHER_FSSCHEDULER_H
#define FEATHER_FSSCHEDULER_H

#include <cstdint>
#include <array>
#include <deque>
#include <limits>
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

// Clamps a raw user-supplied priority into the valid 1–15 execution range.
// Budget 0 is mapped to 1 so every class has at least one credit per round.
static inline uint8_t normalize_budget(uint8_t p) {
    p &= 0x0F;
    return p == 0 ? static_cast<uint8_t>(1) : p;
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

    // -----------------------------------------------------------------------
    // Instant tasks — pending one-shot callbacks waiting to become runnable.
    // They are moved into budget-indexed ready queues on step().
    // -----------------------------------------------------------------------
    struct InstantTaskRecord {
        FSCallback task;
        uint8_t    budget = 0;  // 4-bit priority value passed by the user (0–15)
        uint64_t   id     = 0;
    };

    std::deque<InstantTaskRecord> instant_task_records;

    // -----------------------------------------------------------------------
    // Timed tasks — single min-heap keyed on next_fire_ms.
    // Holds only not-yet-due tasks.
    //   period_ms == 0  →  one-shot deferred task
    //   period_ms  > 0  →  periodic task
    // -----------------------------------------------------------------------
    struct TimedTaskRecord {
        uint64_t   next_fire_ms = 0;
        FSCallback task;
        uint8_t    budget    = 0;  // 4-bit priority value (0–15)
        uint32_t   period_ms = 0;
        uint64_t   id        = 0;
        FSSchedulerPeriodicTaskRepeatAllocationType repeat_type =
            FSSchedulerPeriodicTaskRepeatAllocationType::Absolute;
    };

    struct TimedTaskCmp {
        bool operator()(const TimedTaskRecord& a, const TimedTaskRecord& b) const {
            return a.next_fire_ms > b.next_fire_ms;
        }
    };

    std::priority_queue<TimedTaskRecord,
                        std::vector<TimedTaskRecord>,
                        TimedTaskCmp> timed_heap;

    // -----------------------------------------------------------------------
    // Ready queues (0..15 budget levels):
    // - due timed tasks and instant tasks are enqueued here
    // - selection uses per-class credit (deficit style round refill)
    // -----------------------------------------------------------------------
    struct ReadyTaskRecord {
        FSCallback task;
        uint8_t    base_budget  = 0;  // user-provided fixed budget (0-15)
        uint32_t   period_ms    = 0;  // 0 for one-shot, >0 for periodic
        uint64_t   next_fire_ms = 0;  // used only when period_ms > 0
        uint64_t   id           = 0;
        FSSchedulerPeriodicTaskRepeatAllocationType repeat_type =
            FSSchedulerPeriodicTaskRepeatAllocationType::Absolute;
    };

    std::array<std::deque<ReadyTaskRecord>, 16> ready_queues{};
    std::array<uint8_t, 16> class_credit{};
    uint16_t ready_bitmap = 0;
    uint8_t  rr_cursor    = 0;  // last-dispatched budget class; cyclic search starts here+1
    // Dispatch count per step(). Keep 1 for compatibility; increase to reduce burst latency.
    static constexpr uint32_t DISPATCH_PER_STEP = 1;

    uint64_t next_wakeup_time = 0;
    uint64_t next_id          = 1;

    void maybe_shrink_timed_heap();
    void enqueue_ready_task(ReadyTaskRecord&& record);
    bool has_ready_tasks() const;
    int  next_ready_budget_with_credit();
    void refill_ready_credits();
    bool pop_next_ready_task(ReadyTaskRecord& out);

public:

    explicit FSScheduler(FSTime& clock_src);

    // -----------------------------------------------------------------------
    // Template API
    //
    // Each method accepts any void() callable directly — lambdas with
    // local-variable captures, functors, etc. — and wraps it into an
    // FSCallback. The external name is "priority"; internally it is stored
    // as "budget" (4-bit, 0–15).
    // -----------------------------------------------------------------------

    template<typename F>
    uint64_t add_instant_task(F&& task, uint8_t priority) {
        const uint64_t id = next_id++;
        instant_task_records.push_back(
            InstantTaskRecord{
                FSCallback(std::forward<F>(task)),
                normalize_budget(priority),
                id
            }
        );
        return id;
    }

    template<typename F>
    uint64_t add_deferred_task(F&& task, uint64_t timestamp_ms, uint8_t priority) {
        const uint64_t id = next_id++;
        TimedTaskRecord rec{
            timestamp_ms,
            FSCallback(std::forward<F>(task)),
            normalize_budget(priority),
            0u,
            id,
            FSSchedulerPeriodicTaskRepeatAllocationType::Absolute
        };
        timed_heap.push(std::move(rec));
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
            FSCallback(std::forward<F>(task)),
            normalize_budget(priority),
            period_ms,
            id,
            allocation_type
        };
        timed_heap.push(std::move(rec));
        return id;
    }

    uint64_t calculate_next_wakeup_time_ms(uint64_t now_ms);
    uint64_t get_next_wakeup_time_ms() const;
    void     step();
};

#endif // FEATHER_FSSCHEDULER_H
