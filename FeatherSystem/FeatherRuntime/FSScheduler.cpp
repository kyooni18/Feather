#include "FSScheduler.hpp"
#include <limits>
#include <numeric>

FSScheduler::FSScheduler(FSTime& clock_src)
    : clock(clock_src)
    , instant_task_records{}
    , instant_tasks{}
    , instant_task_budgets{}
    , instant_task_ids{}
    , instant_rr_cursor{0}
    , instant_cycle_dirty{true}
    , timed_heap{}
    , next_wakeup_time{0}
    , next_id{1} {
}

void FSScheduler::rebuild_instant_schedule_if_dirty() {
    if (!instant_cycle_dirty) {
        return;
    }
    instant_cycle_dirty = false;

    instant_tasks.clear();
    instant_task_budgets.clear();
    instant_task_ids.clear();

    const size_t n = instant_task_records.size();
    if (n == 0) {
        return;
    }

    std::vector<uint8_t> weights;
    weights.reserve(n);
    uint64_t total_weight = 0;
    for (const auto& rec : instant_task_records) {
        const uint8_t w = fs_budget_weight(rec.budget);
        weights.push_back(w);
        total_weight += w;
    }
    if (total_weight == 0) {
        return;
    }

    uint64_t g = 0;
    for (uint8_t w : weights) {
        if (w == 0) continue;
        g = (g == 0) ? w : std::gcd(g, static_cast<uint64_t>(w));
    }
    if (g > 1) {
        total_weight = 0;
        for (auto& w : weights) {
            w = static_cast<uint8_t>(w / static_cast<uint8_t>(g));
            total_weight += w;
        }
    }

    std::vector<int64_t> current(n, 0);
    size_t cursor = (instant_rr_cursor < n) ? instant_rr_cursor : 0;

    for (uint64_t slot = 0; slot < total_weight; ++slot) {
        for (size_t i = 0; i < n; ++i) {
            current[i] += static_cast<int64_t>(weights[i]);
        }

        size_t  best_i   = 0;
        int64_t best_val = std::numeric_limits<int64_t>::min();
        bool    best_set = false;

        for (size_t step = 0; step < n; ++step) {
            const size_t i = (cursor + 1 + step) % n;
            if (weights[i] == 0) continue;
            if (!best_set || current[i] > best_val) {
                best_set = true;
                best_val = current[i];
                best_i   = i;
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
    if (weight == 0) return 0;

    const uint64_t id = next_id++;
    instant_task_records.push_back(InstantTaskRecord{task, budget, id});
    instant_cycle_dirty = true;
    return id;
}

uint64_t FSScheduler::add_deferred_task(void (*task)(...), uint64_t timestamp_ms, uint8_t budget) {
    const uint64_t id = next_id++;
    timed_heap.push(TimedTaskRecord{
        timestamp_ms, task, budget,
        /*period_ms=*/0, id,
        FSSchedulerPeriodicTaskRepeatAllocationType::Absolute
    });
    return id;
}

uint64_t FSScheduler::add_periodic_task(
    void (*task)(...),
    uint64_t start_timestamp_ms,
    uint32_t period_ms,
    uint8_t  budget,
    FSSchedulerPeriodicTaskRepeatAllocationType allocation_type
) {
    const uint64_t id = next_id++;
    timed_heap.push(TimedTaskRecord{
        start_timestamp_ms, task, budget,
        period_ms, id, allocation_type
    });
    return id;
}

uint64_t FSScheduler::calculate_next_wakeup_time_ms(uint64_t now_ms) {
    if (timed_heap.empty()) {
        next_wakeup_time = 0;
        return 0;
    }

    const uint64_t earliest = timed_heap.top().next_fire_ms;
    next_wakeup_time = (earliest <= now_ms) ? now_ms : earliest;
    return next_wakeup_time;
}

uint64_t FSScheduler::get_next_wakeup_time_ms() const {
    return next_wakeup_time;
}

const std::vector<uint64_t>& FSScheduler::debug_instant_task_ids() const {
    const_cast<FSScheduler*>(this)->rebuild_instant_schedule_if_dirty();
    return instant_task_ids;
}

const std::vector<uint8_t>& FSScheduler::debug_instant_task_budgets() const {
    const_cast<FSScheduler*>(this)->rebuild_instant_schedule_if_dirty();
    return instant_task_budgets;
}
