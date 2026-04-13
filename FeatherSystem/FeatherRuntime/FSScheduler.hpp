#ifndef FEATHER_FSSCHEDULER_H
#define FEATHER_FSSCHEDULER_H
#include <vector>
#include <queue>
#include <cstdint>
#include "FSTime.hpp"

static inline uint8_t fs_budget_weight(uint8_t budget) {
    return static_cast<uint8_t>((budget >> 4) & 0x0F);
}

static inline uint8_t fs_budget_quantum(uint8_t budget) {
    return static_cast<uint8_t>(budget & 0x0F);
}

static inline uint8_t fs_budget_pack(uint8_t weight, uint8_t quantum) {
    return static_cast<uint8_t>(((weight & 0x0F) << 4) | (quantum & 0x0F));
}

enum FSSchedulerPeriodicTaskRepeatAllocationType {
    Relative, Absolute
};

class FSScheduler {
    private:

    FSTime clock;

    // --- Instant tasks (weighted round-robin) ---

    struct InstantTaskRecord {
        void (*task)(...) = nullptr;
        uint8_t budget = 0;  // high nibble: weight, low nibble: quantum/cost
        uint64_t id = 0;
    };

    std::vector<InstantTaskRecord> instant_task_records;

    // Rebuilt lazily from instant_task_records when instant_cycle_dirty is set.
    std::vector<void (*)(...)> instant_tasks;
    std::vector<uint8_t>       instant_task_budgets;
    std::vector<uint64_t>      instant_task_ids;
    size_t instant_rr_cursor = 0;
    bool   instant_cycle_dirty = true;

    void rebuild_instant_schedule_if_dirty();

    // --- Timed tasks (deferred + periodic) ---
    //
    // Single min-heap keyed on next_fire_ms.
    // period_ms == 0  →  one-shot deferred task
    // period_ms  > 0  →  periodic task
    //
    // O(log n) insert/reschedule, O(1) peek at the earliest deadline.
    // No parallel vectors, no secondary timestamp-only heap.

    struct TimedTaskRecord {
        uint64_t next_fire_ms = 0;
        void (*task)(...) = nullptr;
        uint8_t  budget    = 0;
        uint32_t period_ms = 0;
        uint64_t id        = 0;
        FSSchedulerPeriodicTaskRepeatAllocationType repeat_type =
            FSSchedulerPeriodicTaskRepeatAllocationType::Absolute;
    };

    struct TimedTaskCmp {
        // Makes std::priority_queue a min-heap on next_fire_ms.
        bool operator()(const TimedTaskRecord& a, const TimedTaskRecord& b) const {
            return a.next_fire_ms > b.next_fire_ms;
        }
    };

    std::priority_queue<TimedTaskRecord,
                        std::vector<TimedTaskRecord>,
                        TimedTaskCmp> timed_heap;

    uint64_t next_wakeup_time = 0;
    uint64_t next_id = 1;

    public:

    FSScheduler(FSTime& clock_src);

    uint64_t add_instant_task(void (*task)(...), uint8_t budget);

    uint64_t add_deferred_task(void (*task)(...), uint64_t timestamp_ms, uint8_t budget);

    uint64_t add_periodic_task(
        void (*task)(...),
        uint64_t start_timestamp_ms,
        uint32_t period_ms,
        uint8_t budget,
        FSSchedulerPeriodicTaskRepeatAllocationType allocation_type =
            FSSchedulerPeriodicTaskRepeatAllocationType::Absolute
    );

    uint64_t calculate_next_wakeup_time_ms(uint64_t now_ms);

    uint64_t get_next_wakeup_time_ms() const;


    const std::vector<uint64_t>& debug_instant_task_ids() const;
    const std::vector<uint8_t>&  debug_instant_task_budgets() const;
};

#endif //FEATHER_FSSCHEDULER_H
