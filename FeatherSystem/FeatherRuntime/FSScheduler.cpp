#include "FSScheduler.hpp"

FSScheduler::FSScheduler(FSTime& clock_src)
    : clock(clock_src)
    , ready_task_queue{}
    , next_ready_sequence{0}
    , instant_task_records{}
    , pending_instant_task_records{}
    , instant_rr_cursor{0}
    , instant_rr_budget_remaining{0}
    , active_instant_task_count{0}
    , instant_dispatch_depth{0}
    , timed_heap{}
    , cancelled_timed_task_count{0}
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
        state_it->second.in_timed_heap = false;
        if (cancelled_timed_task_count > 0) {
            --cancelled_timed_task_count;
        }
        timed_task_states.erase(state_it);
    }
}

void FSScheduler::rebuild_cancelled_timed_heap() {
    decltype(timed_heap) rebuilt;

    while (!timed_heap.empty()) {
        TimedTaskRecord record = timed_heap.top();
        timed_heap.pop();

        auto state_it = timed_task_states.find(record.id);
        if (state_it == timed_task_states.end()) {
            continue;
        }
        if (state_it->second.cancelled) {
            state_it->second.in_timed_heap = false;
            timed_task_states.erase(state_it);
            continue;
        }

        rebuilt.push(record);
    }

    timed_heap.swap(rebuilt);
    cancelled_timed_task_count = 0;
}

void FSScheduler::maybe_rebuild_cancelled_timed_heap() {
    if (cancelled_timed_task_count == 0 || timed_heap.empty()) {
        return;
    }

    const size_t min_cancelled_before_rebuild = 16;
    const bool enough_cancelled_tasks =
        cancelled_timed_task_count >= min_cancelled_before_rebuild;
    const bool heap_is_mostly_cancelled =
        cancelled_timed_task_count * 2u >= timed_heap.size();

    if (!enough_cancelled_tasks && !heap_is_mostly_cancelled) {
        return;
    }

    rebuild_cancelled_timed_heap();
}

void FSScheduler::enqueue_ready_callback(FSCallback&& task, uint8_t priority) {
    ready_task_queue.push(
        ReadyTaskRecord{
            std::move(task),
            normalize_budget(priority),
            next_ready_sequence++
        }
    );
}

bool FSScheduler::run_one_ready_task() {
    if (ready_task_queue.empty()) {
        return false;
    }

    ReadyTaskRecord record =
        std::move(const_cast<ReadyTaskRecord&>(ready_task_queue.top()));
    ready_task_queue.pop();
    if (record.task) {
        record.task();
    }
    return true;
}

bool FSScheduler::run_one_instant_task() {
    if (instant_dispatch_depth != 0) {
        return false;
    }

    flush_pending_instant_task_records();
    maybe_compact_instant_task_records();

    if (active_instant_task_count == 0 || instant_task_records.empty()) {
        return false;
    }

    size_t checked_records = 0;
    while (checked_records < instant_task_records.size()) {
        if (instant_rr_cursor >= instant_task_records.size()) {
            instant_rr_cursor = 0;
            instant_rr_budget_remaining = 0;
        }

        auto& rec = instant_task_records[instant_rr_cursor];
        if (!rec.active || !rec.task) {
            instant_rr_budget_remaining = 0;
            instant_rr_cursor = (instant_rr_cursor + 1) % instant_task_records.size();
            ++checked_records;
            continue;
        }

        if (instant_rr_budget_remaining == 0) {
            instant_rr_budget_remaining = slices_for_budget(rec.budget);
        }

        rec.dispatching = true;
        ++instant_dispatch_depth;
        rec.task();
        --instant_dispatch_depth;
        rec.dispatching = false;

        if (!rec.active || !rec.task) {
            rec.task = FSCallback{};
            instant_rr_budget_remaining = 0;
            instant_rr_cursor = (instant_rr_cursor + 1) % instant_task_records.size();
            flush_pending_instant_task_records();
            maybe_compact_instant_task_records();
            return true;
        }

        --instant_rr_budget_remaining;
        if (instant_rr_budget_remaining == 0) {
            instant_rr_cursor = (instant_rr_cursor + 1) % instant_task_records.size();
        }
        flush_pending_instant_task_records();
        maybe_compact_instant_task_records();
        return true;
    }

    active_instant_task_count = 0;
    instant_rr_cursor = 0;
    instant_rr_budget_remaining = 0;
    return false;
}

void FSScheduler::flush_pending_instant_task_records() {
    if (pending_instant_task_records.empty() || instant_dispatch_depth != 0) {
        return;
    }

    for (auto& record : pending_instant_task_records) {
        if (record.active && record.task) {
            ++active_instant_task_count;
        }
        instant_task_records.push_back(std::move(record));
    }
    pending_instant_task_records.clear();
}

void FSScheduler::maybe_compact_instant_task_records() {
    if (instant_dispatch_depth != 0) {
        return;
    }

    if (active_instant_task_count == 0) {
        instant_task_records.clear();
        instant_task_records.shrink_to_fit();
        instant_rr_cursor = 0;
        instant_rr_budget_remaining = 0;
        return;
    }

    if (instant_task_records.empty()) {
        instant_rr_cursor = 0;
        instant_rr_budget_remaining = 0;
        return;
    }

    size_t active_count = 0;
    for (const auto& record : instant_task_records) {
        if (record.active && record.task) {
            ++active_count;
        }
    }

    active_instant_task_count = active_count;
    if (active_count == 0) {
        instant_task_records.clear();
        instant_task_records.shrink_to_fit();
        instant_rr_cursor = 0;
        instant_rr_budget_remaining = 0;
        return;
    }

    const size_t inactive_count = instant_task_records.size() - active_count;
    const bool enough_inactive_records = inactive_count >= 16u;
    const bool records_are_mostly_inactive =
        inactive_count * 2u >= instant_task_records.size();

    if (!enough_inactive_records && !records_are_mostly_inactive) {
        if (instant_rr_cursor >= instant_task_records.size()) {
            instant_rr_cursor = 0;
            instant_rr_budget_remaining = 0;
        }
        return;
    }

    const size_t old_cursor = instant_rr_cursor;
    size_t active_before_cursor = 0;
    const size_t cursor_limit =
        (old_cursor < instant_task_records.size()) ? old_cursor : instant_task_records.size();
    for (size_t i = 0; i < cursor_limit; ++i) {
        if (instant_task_records[i].active && instant_task_records[i].task) {
            ++active_before_cursor;
        }
    }

    size_t write_index = 0;
    for (size_t read_index = 0; read_index < instant_task_records.size(); ++read_index) {
        auto& record = instant_task_records[read_index];
        if (!record.active || !record.task) {
            continue;
        }

        if (write_index != read_index) {
            instant_task_records[write_index] = std::move(record);
        }
        ++write_index;
    }
    instant_task_records.resize(write_index);
    active_instant_task_count = write_index;
    instant_rr_cursor = (active_before_cursor < write_index) ? active_before_cursor : 0;
    instant_rr_budget_remaining = 0;

    if (instant_task_records.size() * 4u < instant_task_records.capacity()) {
        instant_task_records.shrink_to_fit();
    }
}

void FSScheduler::invoke_timed_ready_task(uint64_t task_id, uint32_t dispatch_epoch) {
    auto state_it = timed_task_states.find(task_id);
    if (state_it == timed_task_states.end()) {
        return;
    }

    auto& state = state_it->second;
    if (state.dispatch_epoch != dispatch_epoch || !state.dispatch_pending) {
        if (state.cancelled && !state.in_timed_heap) {
            timed_task_states.erase(state_it);
        }
        return;
    }

    state.dispatch_pending = false;

    if (!state.cancelled && state.enabled && state.task) {
        state.task();
    }

    if (!state.is_periodic) {
        timed_task_states.erase(state_it);
    } else if (state.cancelled && !state.in_timed_heap) {
        timed_task_states.erase(state_it);
    }
}

void FSScheduler::promote_due_timed_tasks(uint64_t now_ms) {
    prune_cancelled_timed_tasks();

    while (!timed_heap.empty() && timed_heap.top().next_fire_ms <= now_ms) {
        TimedTaskRecord record = timed_heap.top();
        timed_heap.pop();

        auto state_it = timed_task_states.find(record.id);
        if (state_it == timed_task_states.end()) {
            continue;
        }

        auto& state = state_it->second;
        state.in_timed_heap = false;

        if (state.cancelled) {
            if (cancelled_timed_task_count > 0) {
                --cancelled_timed_task_count;
            }
            timed_task_states.erase(state_it);
            continue;
        }

        if (state.enabled && !state.dispatch_pending && state.task) {
            state.dispatch_pending = true;
            const uint32_t dispatch_epoch = state.dispatch_epoch;
            enqueue_ready_task(
                [this, task_id = record.id, dispatch_epoch]() {
                    invoke_timed_ready_task(task_id, dispatch_epoch);
                },
                state.budget
            );
        }

        if (state.is_periodic && state.period_ms != 0u) {
            uint64_t next_fire_ms;
            if (state.repeat_type == Absolute) {
                const uint64_t base_fire = record.next_fire_ms;
                const uint64_t period = state.period_ms;
                if (base_fire > now_ms) {
                    next_fire_ms = base_fire;
                } else {
                    const uint64_t elapsed = now_ms - base_fire;
                    const uint64_t skips = (elapsed / period) + 1u;
                    next_fire_ms = base_fire + (skips * period);
                }
            } else {
                next_fire_ms = now_ms + state.period_ms;
            }

            record.next_fire_ms = next_fire_ms;
            record.budget = state.budget;
            timed_heap.push(record);
            state.in_timed_heap = true;
        } else if (!state.dispatch_pending) {
            timed_task_states.erase(state_it);
        }
    }

    maybe_rebuild_cancelled_timed_heap();
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
        if (!instant_task_records[i].active || instant_task_records[i].id != task_id) {
            continue;
        }

        instant_task_records[i].active = false;
        if (!instant_task_records[i].dispatching) {
            instant_task_records[i].task = FSCallback{};
        }
        if (active_instant_task_count > 0) {
            --active_instant_task_count;
        }
        if (active_instant_task_count == 0) {
            instant_rr_cursor = 0;
            instant_rr_budget_remaining = 0;
        } else {
            instant_rr_budget_remaining = 0;
            if (instant_rr_cursor >= instant_task_records.size()) {
                instant_rr_cursor = 0;
            }
        }
        maybe_compact_instant_task_records();
        return true;
    }

    for (auto& record : pending_instant_task_records) {
        if (!record.active || record.id != task_id) {
            continue;
        }

        record.active = false;
        record.task = FSCallback{};
        return true;
    }

    auto state_it = timed_task_states.find(task_id);
    if (state_it == timed_task_states.end() || state_it->second.cancelled) {
        return false;
    }

    state_it->second.cancelled = true;
    state_it->second.enabled = false;
    if (state_it->second.dispatch_pending) {
        state_it->second.dispatch_pending = false;
        ++state_it->second.dispatch_epoch;
    }
    state_it->second.task = FSCallback{};
    if (state_it->second.in_timed_heap) {
        ++cancelled_timed_task_count;
        maybe_rebuild_cancelled_timed_heap();
    }
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
    if (!enabled && state_it->second.dispatch_pending) {
        state_it->second.dispatch_pending = false;
        ++state_it->second.dispatch_epoch;
    }
    return true;
}

bool FSScheduler::is_task_enabled(uint64_t task_id) const {
    for (const auto& record : instant_task_records) {
        if (record.active && record.id == task_id) {
            return true;
        }
    }
    for (const auto& record : pending_instant_task_records) {
        if (record.active && record.id == task_id) {
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

    promote_due_timed_tasks(now_ms);

    if (!run_one_ready_task()) {
        run_one_instant_task();
    }

    maybe_shrink_timed_heap();
    calculate_next_wakeup_time_ms(now_ms);
}
