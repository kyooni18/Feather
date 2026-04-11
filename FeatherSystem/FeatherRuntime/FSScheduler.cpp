#include "FSScheduler.hpp"
#include <cstdio>
#include <limits>

FSScheduler::FSScheduler(FSTime& clock_src)
    : clock(clock_src)
    , instant_tasks{}
    , instant_task_budgets{}
    , instant_task_ids{}
    , instant_task_records{}
    , timed_tasks{}
    , timed_task_timestamps{}
    , timed_task_periods{}
    , timed_task_budgets{}
    , timed_task_ids{}
    , timed_task_types{}
    , timed_task_repeat_allocation_types{}
    , periodic_tasks{}
    , next_wakeup_time{0} {
}

void FSScheduler::rebuild_instant_schedule() {
    instant_tasks.clear();
    instant_task_budgets.clear();
    instant_task_ids.clear();

    // Interleaved allocation using smooth weighted round-robin.
    // Build one "cycle" of length sum(weights). Execution stays simple
    // (linear scan) because insertion does the ordering work.
    const size_t n = instant_task_records.size();
    if (n == 0) return;

    std::vector<uint8_t> weights;
    weights.reserve(n);
    uint64_t total_weight = 0;
    for (const auto& rec : instant_task_records) {
        const uint8_t w = fs_budget_weight(rec.budget);
        weights.push_back(w);
        total_weight += w;
    }
    if (total_weight == 0) return;

    std::vector<int64_t> current;
    current.assign(n, 0);

    size_t cursor = (instant_rr_cursor < n) ? instant_rr_cursor : 0;

    for (uint64_t slot = 0; slot < total_weight; ++slot) {
        // Increase current weights.
        for (size_t i = 0; i < n; ++i) {
            current[i] += static_cast<int64_t>(weights[i]);
        }

        // Choose max current with round-robin tie-break.
        size_t best_i = 0;
        int64_t best_val = std::numeric_limits<int64_t>::min();
        bool best_set = false;

        for (size_t step = 0; step < n; ++step) {
            const size_t i = (cursor + 1 + step) % n;
            if (weights[i] == 0) continue;
            const int64_t v = current[i];
            if (!best_set || v > best_val) {
                best_set = true;
                best_val = v;
                best_i = i;
            }
        }
        if (!best_set) break;

        current[best_i] -= static_cast<int64_t>(total_weight);

        const auto& rec = instant_task_records[best_i];
        instant_tasks.push_back(rec.task);
        instant_task_budgets.push_back(rec.budget);
        instant_task_ids.push_back(rec.id);

        cursor = best_i;
    }

    instant_rr_cursor = cursor;
}

uint64_t FSScheduler::add_instant_task(void (*task)(...), uint8_t budget) {
    const uint8_t weight = fs_budget_weight(budget);
    if (weight == 0) {
        return 0;
    }

    const uint64_t id = next_id++;
    instant_task_records.push_back(InstantTaskRecord{task, budget, id});
    rebuild_instant_schedule();
    return id;
}

uint64_t FSScheduler::add_deferred_task(void (*task)(...), uint64_t timestamp_ms, uint8_t budget) {
    const uint64_t id = next_id++;
    timed_tasks.push_back(task);
    timed_task_timestamps.push_back(timestamp_ms);
    timed_task_periods.push_back(0);
    timed_task_budgets.push_back(budget);
    timed_task_ids.push_back(id);
    timed_task_types.push_back(TimedTaskType::Deferred);
    timed_task_repeat_allocation_types.push_back(FSSchedulerPeriodicTaskRepeatAllocationType::Absolute);
    return id;
}

uint64_t FSScheduler::add_periodic_task(
    void (*task)(...),
    uint64_t start_timestamp_ms,
    uint32_t period_ms,
    uint8_t budget,
    FSSchedulerPeriodicTaskRepeatAllocationType allocation_type
) {
    const uint64_t id = next_id++;
    timed_tasks.push_back(task);
    timed_task_timestamps.push_back(start_timestamp_ms);
    timed_task_periods.push_back(period_ms);
    timed_task_budgets.push_back(budget);
    timed_task_ids.push_back(id);
    timed_task_types.push_back(TimedTaskType::Periodic);
    timed_task_repeat_allocation_types.push_back(allocation_type);

    periodic_tasks.push_back(FSSchedulerPeriodicTask(task, budget, period_ms, start_timestamp_ms, allocation_type));
    periodic_tasks.back().id = id;
    return id;
}

uint64_t FSScheduler::calculate_next_wakeup_time_ms(uint64_t now_ms) {
    if (timed_task_timestamps.empty()) {
        next_wakeup_time = 0;
        return next_wakeup_time;
    }

    uint64_t best = std::numeric_limits<uint64_t>::max();
    for (uint64_t ts : timed_task_timestamps) {
        if (ts <= now_ms) {
            best = now_ms;
            break;
        }
        if (ts < best) best = ts;
    }

    next_wakeup_time = (best == std::numeric_limits<uint64_t>::max()) ? 0 : best;
    return next_wakeup_time;
}

uint64_t FSScheduler::get_next_wakeup_time_ms() const {
    return next_wakeup_time;
}

const std::vector<uint64_t>& FSScheduler::debug_instant_task_ids() const {
    return instant_task_ids;
}

const std::vector<uint8_t>& FSScheduler::debug_instant_task_budgets() const {
    return instant_task_budgets;
}