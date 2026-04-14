#pragma once

#ifndef FEATHER_FSSCHEDULER_H
#define FEATHER_FSSCHEDULER_H

#include <cstdint>
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

    // -----------------------------------------------------------------------
    // Instant tasks — stored once in a flat vector; executed by a simple
    // round-robin cursor directly on the original records.
    // No expansion arrays, no rebuild phase, no weighted-RR overhead.
    // -----------------------------------------------------------------------
    struct InstantTaskRecord {
        FSCallback task;
        uint8_t    budget = 0;  // 4-bit priority value passed by the user (0–15)
        uint64_t   id     = 0;
    };

    std::vector<InstantTaskRecord> instant_task_records;
    size_t instant_rr_cursor = 0;

    // -----------------------------------------------------------------------
    // Timed tasks — single min-heap keyed on next_fire_ms.
    //   period_ms == 0  →  one-shot deferred task
    //   period_ms  > 0  →  periodic task
    // -----------------------------------------------------------------------
    struct TimedTaskRecord {
        uint64_t   next_fire_ms = 0;
        uint64_t   id = 0;
        FSCallback task;
        uint32_t   period_ms = 0;
        uint8_t    repeat_type = static_cast<uint8_t>(FSSchedulerPeriodicTaskRepeatAllocationType::Absolute);
    };

    struct TimedTaskCmp {
        bool operator()(const TimedTaskRecord& a, const TimedTaskRecord& b) const {
            return a.next_fire_ms > b.next_fire_ms;
        }
    };

    std::priority_queue<TimedTaskRecord,
                        std::vector<TimedTaskRecord>,
                        TimedTaskCmp> timed_heap;

    uint64_t next_wakeup_time = 0;
    uint64_t next_id          = 1;

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
                static_cast<uint8_t>(priority & 0x0F),
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
            id,
            FSCallback(std::forward<F>(task)),
            0u,
            static_cast<uint8_t>(FSSchedulerPeriodicTaskRepeatAllocationType::Absolute)
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
            id,
            FSCallback(std::forward<F>(task)),
            period_ms,
            static_cast<uint8_t>(allocation_type)
        };
        timed_heap.push(std::move(rec));
        return id;
    }

    uint64_t calculate_next_wakeup_time_ms(uint64_t now_ms);
    uint64_t get_next_wakeup_time_ms() const;
    void     step();
};

#endif // FEATHER_FSSCHEDULER_H

