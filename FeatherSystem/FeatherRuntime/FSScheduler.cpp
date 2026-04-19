#include "FSScheduler.hpp"

FSScheduler::FSScheduler(FSTime& clock_src)
    : clock(clock_src)
    , instant_task_records{}
    , instant_rr_cursor{0}
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

void FSScheduler::step() {
    const uint64_t now_ms = clock.now_ms();

    if (!instant_task_records.empty()) {
        if (instant_rr_cursor >= instant_task_records.size()) {
            instant_rr_cursor = 0;
        }
        auto& rec = instant_task_records[instant_rr_cursor];
        if (rec.task) rec.task();
        instant_task_records.erase(instant_task_records.begin() + instant_rr_cursor);
        if (instant_task_records.empty() || instant_rr_cursor >= instant_task_records.size()) {
            instant_rr_cursor = 0;
        }
    }

    while (!timed_heap.empty() && timed_heap.top().next_fire_ms <= now_ms) {
        TimedTaskRecord record =
            std::move(const_cast<TimedTaskRecord&>(timed_heap.top()));
        timed_heap.pop();

        if (record.task) record.task();

        if (record.period_ms != 0) {
            uint64_t next_fire_ms;
            if (record.repeat_type == static_cast<uint8_t>(Absolute)) {
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
        }
    }

    maybe_shrink_timed_heap();
    calculate_next_wakeup_time_ms(now_ms);
}
