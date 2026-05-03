#include "FSScheduler.hpp"

FSScheduler::FSScheduler(FSTime& clock_src)
    : clock(clock_src)
    , ready_task_queue{}
    , next_ready_sequence{0}
    , instant_task_states{}
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

void FSScheduler::enqueue_ready_callback(FSCallback&& task, uint8_t priority, uint64_t task_id, bool is_instant) {
    ready_task_queue.push(
        ReadyTaskRecord{
            std::move(task),
            normalize_budget(priority),
            next_ready_sequence++,
            task_id,
            is_instant
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
    if (record.is_instant) {
        auto instant_state = instant_task_states.find(record.task_id);
        if (instant_state == instant_task_states.end()) {
            return true;
        }
        const bool enabled = instant_state->second;
        instant_task_states.erase(instant_state);
        if (enabled && record.task) {
            record.task();
        }
        return true;
    }

    if (record.task) {
        record.task();
    }
    return true;
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
    auto instant_state = instant_task_states.find(task_id);
    if (instant_state != instant_task_states.end()) {
        instant_task_states.erase(instant_state);
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
    auto instant_state = instant_task_states.find(task_id);
    if (instant_state != instant_task_states.end()) {
        return instant_state->second;
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

    run_one_ready_task();

    maybe_shrink_timed_heap();
    calculate_next_wakeup_time_ms(now_ms);
}
