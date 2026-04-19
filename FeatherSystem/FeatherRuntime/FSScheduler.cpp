#include "FSScheduler.hpp"

FSScheduler::FSScheduler(FSTime& clock_src)
    : clock(clock_src)
    , instant_task_records{}
    , timed_heap{}
    , ready_queues{}
    , class_credit{}
    , ready_bitmap{0}
    , next_wakeup_time{0}
    , next_id{1} {
}

void FSScheduler::enqueue_ready_task(ReadyTaskRecord&& record) {
    const uint8_t queue_index = static_cast<uint8_t>(record.base_budget & 0x0F);
    ready_queues[queue_index].push_back(std::move(record));
    ready_bitmap |= static_cast<uint16_t>(1u << queue_index);
}

bool FSScheduler::has_ready_tasks() const {
    return ready_bitmap != 0;
}

int FSScheduler::highest_ready_budget_with_credit() {
    for (int budget = 15; budget >= 0; --budget) {
        if ((ready_bitmap & static_cast<uint16_t>(1u << budget)) == 0) {
            continue;
        }
        if (class_credit[static_cast<size_t>(budget)] > 0) {
            return budget;
        }
    }
    return -1;
}

void FSScheduler::refill_ready_credits() {
    for (int budget = 0; budget <= 15; ++budget) {
        if ((ready_bitmap & static_cast<uint16_t>(1u << budget)) != 0) {
            class_credit[static_cast<size_t>(budget)] = static_cast<uint8_t>(budget & 0x0F);
        }
    }
}

bool FSScheduler::pop_next_ready_task(ReadyTaskRecord& out) {
    if (!has_ready_tasks()) {
        return false;
    }

    int budget = highest_ready_budget_with_credit();
    if (budget < 0) {
        refill_ready_credits();
        budget = highest_ready_budget_with_credit();
    }

    if (budget < 0 && ready_bitmap != 0) {
        for (int i = 15; i >= 0; --i) {
            if ((ready_bitmap & static_cast<uint16_t>(1u << i)) != 0) {
                budget = i;
                break;
            }
        }
    }

    if (budget < 0) {
        return false;
    }

    auto& queue = ready_queues[static_cast<size_t>(budget)];
    out = std::move(queue.front());
    queue.pop_front();
    if (class_credit[static_cast<size_t>(budget)] > 0) {
        class_credit[static_cast<size_t>(budget)] =
            static_cast<uint8_t>(class_credit[static_cast<size_t>(budget)] - 1u);
    }
    if (queue.empty()) {
        ready_bitmap &= static_cast<uint16_t>(~(1u << budget));
        class_credit[static_cast<size_t>(budget)] = 0;
    }

    return true;
}

void FSScheduler::maybe_shrink_timed_heap() {
    if (!timed_heap.empty()) {
        return;
    }

    decltype(timed_heap) compacted;
    timed_heap.swap(compacted);
}

uint64_t FSScheduler::calculate_next_wakeup_time_ms(uint64_t now_ms) {
    if (has_ready_tasks() || !instant_task_records.empty()) {
        next_wakeup_time = now_ms;
        return now_ms;
    }

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

void FSScheduler::step() {
    const uint64_t now_ms = clock.now_ms();

    while (!instant_task_records.empty()) {
        InstantTaskRecord record = std::move(instant_task_records.front());
        instant_task_records.pop_front();
        const uint8_t budget_value = static_cast<uint8_t>(record.budget & 0x0F);
        enqueue_ready_task(ReadyTaskRecord{
            std::move(record.task),
            budget_value,
            0u,
            0u,
            record.id,
            FSSchedulerPeriodicTaskRepeatAllocationType::Absolute
        });
    }

    while (!timed_heap.empty() && timed_heap.top().next_fire_ms <= now_ms) {
        TimedTaskRecord record =
            std::move(const_cast<TimedTaskRecord&>(timed_heap.top()));
        timed_heap.pop();
        const uint8_t budget_value = static_cast<uint8_t>(record.budget & 0x0F);
        enqueue_ready_task(ReadyTaskRecord{
            std::move(record.task),
            budget_value,
            record.period_ms,
            record.next_fire_ms,
            record.id,
            record.repeat_type
        });
    }

    for (uint32_t dispatched = 0; dispatched < max_dispatch_per_step; ++dispatched) {
        ReadyTaskRecord selected;
        if (!pop_next_ready_task(selected)) {
            break;
        }
        if (selected.task) selected.task();

        if (selected.period_ms != 0) {
            uint64_t next_fire_ms;
            if (selected.repeat_type == FSSchedulerPeriodicTaskRepeatAllocationType::Absolute) {
                const uint64_t base_fire = selected.next_fire_ms;
                const uint64_t period = selected.period_ms;
                if (base_fire > now_ms) {
                    next_fire_ms = base_fire;
                } else {
                    const uint64_t elapsed = now_ms - base_fire;
                    const uint64_t skips = (elapsed / period) + 1u;
                    next_fire_ms = base_fire + (skips * period);
                }
            } else {
                next_fire_ms = now_ms + selected.period_ms;
            }
            timed_heap.push(TimedTaskRecord{
                next_fire_ms,
                std::move(selected.task),
                static_cast<uint8_t>(selected.base_budget & 0x0F),
                selected.period_ms,
                selected.id,
                selected.repeat_type
            });
        }
    }

    maybe_shrink_timed_heap();
    calculate_next_wakeup_time_ms(now_ms);
}
