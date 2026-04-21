#include "FSScheduler.hpp"

FSScheduler::FSScheduler(FSTime& clock_src)
    : clock(clock_src)
    , ready_task_queue{}
    , next_ready_sequence{0}
    , instant_task_records{}
    , instant_rr_cursor{0}
    , instant_rr_budget_remaining{0}
    , timed_heap{}
    , next_wakeup_time{0}
    , next_id{1} {
}

void FSScheduler::maybe_shrink_timed_heap() {
    if (!timed_heap.empty()) {
        return;
    }

    decltype(timed_heap) compacted;
    timed_heap.swap(compacted);
}

void FSScheduler::prune_cancelled_timed_tasks() {
    while (!timed_heap.empty()) {
        const uint64_t id = timed_heap.top().id;
        auto state_it = timed_task_states.find(id);
        if (state_it == timed_task_states.end()) {
            timed_heap.pop();
            continue;
        }
        if (!state_it->second.cancelled) {
            break;
        }

        timed_heap.pop();
        timed_task_states.erase(state_it);
    }
}

uint64_t FSScheduler::calculate_next_wakeup_time_ms(uint64_t now_ms) {
    if (!ready_task_queue.empty()) {
        next_wakeup_time = now_ms;
        return next_wakeup_time;
    }

    prune_cancelled_timed_tasks();

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

bool FSScheduler::has_ready_tasks() const {
    return !ready_task_queue.empty();
}

bool FSScheduler::cancel_task(uint64_t task_id) {
    for (size_t i = 0; i < instant_task_records.size(); ++i) {
        if (instant_task_records[i].id != task_id) {
            continue;
        }

        instant_task_records.erase(instant_task_records.begin() + i);
        if (instant_task_records.empty()) {
            instant_rr_cursor = 0;
            instant_rr_budget_remaining = 0;
        } else if (i < instant_rr_cursor) {
            --instant_rr_cursor;
        } else if (instant_rr_cursor >= instant_task_records.size()) {
            instant_rr_cursor = 0;
        }
        instant_rr_budget_remaining = 0;
        return true;
    }

    auto state_it = timed_task_states.find(task_id);
    if (state_it == timed_task_states.end() || state_it->second.cancelled) {
        return false;
    }

    state_it->second.cancelled = true;
    state_it->second.enabled = false;
    return true;
}

bool FSScheduler::set_task_enabled(uint64_t task_id, bool enabled) {
    auto state_it = timed_task_states.find(task_id);
    if (state_it == timed_task_states.end()) {
        return false;
    }
    if (state_it->second.cancelled || !state_it->second.is_periodic) {
        return false;
    }

    state_it->second.enabled = enabled;
    return true;
}

bool FSScheduler::is_task_enabled(uint64_t task_id) const {
    for (const auto& record : instant_task_records) {
        if (record.id == task_id) {
            return true;
        }
    }

    auto state_it = timed_task_states.find(task_id);
    if (state_it == timed_task_states.end()) {
        return false;
    }
    if (state_it->second.cancelled) {
        return false;
    }
    return state_it->second.enabled;
}

void FSScheduler::step() {
    const uint64_t now_ms = clock.now_ms();

    prune_cancelled_timed_tasks();

    if (!ready_task_queue.empty()) {
        ReadyTaskRecord record = std::move(const_cast<ReadyTaskRecord&>(ready_task_queue.top()));
        ready_task_queue.pop();
        if (record.task) {
            record.task();
        }
    }

    if (!instant_task_records.empty()) {
        if (instant_rr_cursor >= instant_task_records.size()) {
            instant_rr_cursor = 0;
            instant_rr_budget_remaining = 0;
        }
        auto& rec = instant_task_records[instant_rr_cursor];
        if (instant_rr_budget_remaining == 0) {
            instant_rr_budget_remaining = slices_for_budget(rec.budget);
        }
        if (rec.task) rec.task();
        --instant_rr_budget_remaining;
        if (instant_rr_budget_remaining == 0) {
            instant_rr_cursor = (instant_rr_cursor + 1) % instant_task_records.size();
        }
    }

    while (!timed_heap.empty() && timed_heap.top().next_fire_ms <= now_ms) {
        TimedTaskRecord record =
            std::move(const_cast<TimedTaskRecord&>(timed_heap.top()));
        timed_heap.pop();

        auto state_it = timed_task_states.find(record.id);
        if (state_it == timed_task_states.end()) {
            continue;
        }
        if (state_it->second.cancelled) {
            timed_task_states.erase(state_it);
            continue;
        }

        if (state_it->second.enabled && record.task) record.task();

        if (record.period_ms != 0) {
            uint64_t next_fire_ms;
            if (record.repeat_type == Absolute) {
                const uint64_t base_fire = record.next_fire_ms;
                const uint64_t period = record.period_ms;
                if (base_fire > now_ms) {
                    next_fire_ms = base_fire;
                } else {
                    const uint64_t elapsed = now_ms - base_fire;
                    const uint64_t skips = (elapsed / period) + 1u;
                    next_fire_ms = base_fire + (skips * period);
                }
            } else {
                next_fire_ms = now_ms + record.period_ms;
            }
            record.next_fire_ms = next_fire_ms;
            timed_heap.push(std::move(record));
        } else {
            timed_task_states.erase(state_it);
        }
    }

    maybe_shrink_timed_heap();
    calculate_next_wakeup_time_ms(now_ms);
}
