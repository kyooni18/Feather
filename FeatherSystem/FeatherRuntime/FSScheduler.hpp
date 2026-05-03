#ifndef FEATHER_FSSCHEDULER_H
#define FEATHER_FSSCHEDULER_H

#include <cstddef>
#include <cstdint>
#include <new>
#include <unordered_map>
#include <type_traits>
#include <utility>
#include <vector>
#include <queue>
#include "../Core/Clock.hpp"

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
// Replaces std::function<void()> with a trampoline and small-buffer storage.
// Any void() callable (lambda with captures, functor) can be constructed at
// the call site, with heap fallback only when the callable is too large or not
// safe to move inline.
//
// No virtual dispatch table; invocation is a direct function-pointer call.
// ---------------------------------------------------------------------------
struct FSCallback {
    static constexpr size_t InlineStorageSize = 32;

    alignas(std::max_align_t) unsigned char inline_storage_[InlineStorageSize];
    void*  ctx = nullptr;
    void (*invoke_f)(void*) = nullptr;
    void (*destroy_f)(void*) = nullptr;
    void (*move_f)(void*, void*) = nullptr;
    bool uses_heap_storage = false;

    FSCallback() = default;

    template<typename F,
             typename = std::enable_if_t<!std::is_same_v<std::decay_t<F>, FSCallback>>>
    explicit FSCallback(F&& f) {
        using Decay = std::decay_t<F>;
        invoke_f = [](void* p) { (*static_cast<Decay*>(p))(); };
        destroy_f = [](void* p) { static_cast<Decay*>(p)->~Decay(); };
        move_f = [](void* dst, void* src) {
            new (dst) Decay(std::move(*static_cast<Decay*>(src)));
            static_cast<Decay*>(src)->~Decay();
        };

        if constexpr (fits_inline<Decay>()) {
            ctx = inline_storage_;
            new (ctx) Decay(std::forward<F>(f));
            uses_heap_storage = false;
        } else {
            auto* heap = new Decay(std::forward<F>(f));
            ctx = heap;
            destroy_f = [](void* p) { delete static_cast<Decay*>(p); };
            uses_heap_storage = true;
        }
    }

    ~FSCallback() {
        reset();
    }

    FSCallback(FSCallback&& o) noexcept
        : ctx(nullptr) {
        move_from(std::move(o));
    }

    FSCallback& operator=(FSCallback&& o) noexcept {
        if (this != &o) {
            reset();
            move_from(std::move(o));
        }
        return *this;
    }

    FSCallback(const FSCallback&)            = delete;
    FSCallback& operator=(const FSCallback&) = delete;

    void operator()() const { if (invoke_f) invoke_f(ctx); }
    explicit operator bool() const { return ctx != nullptr && invoke_f != nullptr; }

private:
    template<typename F>
    static constexpr bool fits_inline() {
        return sizeof(F) <= InlineStorageSize &&
               alignof(F) <= alignof(std::max_align_t) &&
               std::is_nothrow_move_constructible_v<F>;
    }

    void reset() {
        if (ctx != nullptr && destroy_f != nullptr) {
            destroy_f(ctx);
        }
        ctx = nullptr;
        invoke_f = nullptr;
        destroy_f = nullptr;
        move_f = nullptr;
        uses_heap_storage = false;
    }

    void move_from(FSCallback&& o) noexcept {
        invoke_f = o.invoke_f;
        destroy_f = o.destroy_f;
        move_f = o.move_f;
        uses_heap_storage = o.uses_heap_storage;

        if (o.ctx == nullptr) {
            ctx = nullptr;
            invoke_f = nullptr;
            destroy_f = nullptr;
            move_f = nullptr;
            uses_heap_storage = false;
            return;
        }

        if (o.uses_heap_storage) {
            ctx = o.ctx;
        } else {
            ctx = inline_storage_;
            move_f(ctx, o.ctx);
        }

        o.ctx = nullptr;
        o.invoke_f = nullptr;
        o.destroy_f = nullptr;
        o.move_f = nullptr;
        o.uses_heap_storage = false;
    }
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

    // -----------------------------------------------------------------------
    // Ready queue — one-shot work scheduled by events or other runtime hooks.
    // Higher budgets run first; matching budgets preserve FIFO order.
    // -----------------------------------------------------------------------
    struct ReadyTaskRecord {
        FSCallback task;
        uint8_t    budget   = 0;
        uint64_t   sequence = 0;
        uint64_t   task_id  = 0;
        bool       is_instant = false;
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

    std::unordered_map<uint64_t, bool> instant_task_states;

    // -----------------------------------------------------------------------
    // Timed tasks — single min-heap keyed on next_fire_ms.
    //   period_ms == 0  →  one-shot deferred task
    //   period_ms  > 0  →  periodic task
    // -----------------------------------------------------------------------
    struct TimedTaskRecord {
        uint64_t   next_fire_ms = 0;
        uint64_t   id        = 0;
        uint8_t    budget    = 0;
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
        FSCallback task;
        uint32_t   period_ms = 0;
        uint8_t    budget    = 0;
        FSSchedulerPeriodicTaskRepeatAllocationType repeat_type =
            FSSchedulerPeriodicTaskRepeatAllocationType::Absolute;
        bool enabled     = true;
        bool cancelled   = false;
        bool is_periodic = false;
        bool dispatch_pending = false;
        uint32_t dispatch_epoch = 0;
        bool in_timed_heap = true;

        TimedTaskState() = default;

        TimedTaskState(
            FSCallback&& callback,
            uint32_t period,
            uint8_t task_budget,
            FSSchedulerPeriodicTaskRepeatAllocationType allocation_type,
            bool periodic
        )
            : task(std::move(callback))
            , period_ms(period)
            , budget(task_budget)
            , repeat_type(allocation_type)
            , enabled(true)
            , cancelled(false)
            , is_periodic(periodic)
            , dispatch_pending(false)
            , dispatch_epoch(0)
            , in_timed_heap(true) {}
    };

    std::priority_queue<TimedTaskRecord,
                        std::vector<TimedTaskRecord>,
                        TimedTaskCmp> timed_heap;
    std::unordered_map<uint64_t, TimedTaskState> timed_task_states;
    size_t cancelled_timed_task_count = 0;

    uint64_t next_wakeup_time = 0;
    uint64_t next_id          = 1;

    void maybe_shrink_timed_heap();
    void prune_cancelled_timed_tasks();
    void maybe_rebuild_cancelled_timed_heap();
    void rebuild_cancelled_timed_heap();
    void promote_due_timed_tasks(uint64_t now_ms);
    void enqueue_ready_callback(FSCallback&& task, uint8_t priority, uint64_t task_id = 0, bool is_instant = false);
    bool run_one_ready_task();
    void invoke_timed_ready_task(uint64_t task_id, uint32_t dispatch_epoch);

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
    // a 4-bit budget. Ready tasks use budget for ordering, instant tasks are
    // enqueued directly into the ready queue, and timed tasks use it to break ties.
    // -----------------------------------------------------------------------

    template<typename F>
    void enqueue_ready_task(F&& task, uint8_t priority) {
        enqueue_ready_callback(FSCallback(std::forward<F>(task)), priority);
    }

    template<typename F>
    uint64_t add_instant_task(F&& task, uint8_t priority) {
        const uint64_t id = next_id++;
        instant_task_states[id] = true;
        enqueue_ready_callback(FSCallback(std::forward<F>(task)), priority, id, true);
        return id;
    }

    template<typename F>
    uint64_t add_deferred_task(F&& task, uint64_t timestamp_ms, uint8_t priority) {
        const uint64_t id = next_id++;
        const uint8_t budget = normalize_budget(priority);
        TimedTaskRecord rec{timestamp_ms, id, budget};
        timed_heap.push(std::move(rec));
        timed_task_states.emplace(
            id,
            TimedTaskState{
                FSCallback(std::forward<F>(task)),
                0u,
                budget,
                FSSchedulerPeriodicTaskRepeatAllocationType::Absolute,
                false
            }
        );
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
        const uint8_t budget = normalize_budget(priority);
        TimedTaskRecord rec{start_timestamp_ms, id, budget};
        timed_heap.push(std::move(rec));
        timed_task_states.emplace(
            id,
            TimedTaskState{
                FSCallback(std::forward<F>(task)),
                period_ms,
                budget,
                allocation_type,
                period_ms != 0u
            }
        );
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
