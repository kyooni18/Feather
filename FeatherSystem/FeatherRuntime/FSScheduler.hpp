#ifndef FEATHER_FSSCHEDULER_H
#define FEATHER_FSSCHEDULER_H
#include <vector>
#include <queue>
#include <cstdint>
#include "FSTime.hpp"

static inline uint8_t fs_budget_translate(uint8_t budget) {
    return static_cast<uint8_t>(budget & 0x0F);
}

static inline uint16_t fs_budget_pair_pack(uint8_t first_budget, uint8_t second_budget) {
    return static_cast<uint16_t>(
        (static_cast<uint16_t>(first_budget & 0x0F) << 4) |
        static_cast<uint16_t>(second_budget & 0x0F)
    );
}

static inline uint8_t fs_budget_pair_get(uint16_t packed_budgets, bool first_budget) {
    if (first_budget) {
        return static_cast<uint8_t>((packed_budgets >> 4) & 0x0F);
    }
    return static_cast<uint8_t>(packed_budgets & 0x0F);
}

enum FSSchedulerPeriodicTaskRepeatAllocationType {
    Relative, Absolute
};

struct FSSchedulerPeriodicTask {
    void (*task)(...) = nullptr;
    FSSchedulerPeriodicTaskRepeatAllocationType repeat_allocation_type = FSSchedulerPeriodicTaskRepeatAllocationType::Absolute;
    uint8_t budget = 0;
    uint32_t period_ms = 0;
    uint64_t start_time_ms = 0;
    uint64_t id = 0;

    FSSchedulerPeriodicTask() = default;
    FSSchedulerPeriodicTask(
        void (*task_to_run)(...),
        uint8_t task_budget,
        uint32_t repeat_period_ms,
        uint64_t task_start_time_ms,
        FSSchedulerPeriodicTaskRepeatAllocationType allocation_type = FSSchedulerPeriodicTaskRepeatAllocationType::Absolute
    ) {
        task = task_to_run;
        budget = task_budget;
        period_ms = repeat_period_ms;
        start_time_ms = task_start_time_ms;
        id = 0;
        repeat_allocation_type = allocation_type;
    }
};

class FSScheduler {
    private:
    enum TimedTaskType {
        Deferred, Periodic
    };

    FSTime clock;

    std::vector<void (*)(...)> instant_tasks;
    std::vector<uint16_t> instant_task_budgets;
    std::vector<uint64_t> instant_task_ids;

    struct InstantTaskRecord {
        void (*task)(...) = nullptr;
        uint8_t budget = 0;
        uint64_t id = 0;
    };

    std::vector<InstantTaskRecord> instant_task_records;
    size_t instant_rr_cursor = 0;

    std::vector<void (*)(...)> timed_tasks;
    std::vector<uint64_t> timed_task_timestamps;
    std::vector<uint32_t> timed_task_periods;
    std::vector<uint8_t> timed_task_budgets;
    std::vector<uint64_t> timed_task_ids; 
    std::vector<TimedTaskType> timed_task_types;
    std::vector<FSSchedulerPeriodicTaskRepeatAllocationType> timed_task_repeat_allocation_types;
    std::vector<FSSchedulerPeriodicTask> periodic_tasks;
    uint64_t next_wakeup_time;
    std::priority_queue<uint64_t, std::vector<uint64_t>, std::greater<uint64_t>> timed_wake_min_heap;

    bool instant_cycle_dirty = true;

    uint64_t next_id = 1;

    void rebuild_instant_schedule_if_dirty();

    public:

    FSScheduler(FSTime& clock_src);

    uint64_t add_instant_task(void (*task)(...), uint8_t budget);

    uint64_t add_deferred_task(void (*task)(...), uint64_t timestamp_ms, uint8_t budget);

    uint64_t add_periodic_task(
        void (*task)(...),
        uint64_t start_timestamp_ms,
        uint32_t period_ms,
        uint8_t budget,
        FSSchedulerPeriodicTaskRepeatAllocationType allocation_type = FSSchedulerPeriodicTaskRepeatAllocationType::Absolute
    );

    uint64_t calculate_next_wakeup_time_ms(uint64_t now_ms);

    uint64_t get_next_wakeup_time_ms() const;

    const std::vector<uint64_t>& debug_instant_task_ids() const;

    const std::vector<uint16_t>& debug_instant_task_budgets() const;
};

#endif //FEATHER_FSSCHEDULER_H
