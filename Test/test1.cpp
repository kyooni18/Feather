#include <Feather.hpp>
#include <cstdint>
#include <iostream>

namespace {
uint64_t fake_now_ms_value = 0;

uint64_t fake_now_ms() {
    return fake_now_ms_value;
}
}

int main() {
    Feather feather(fake_now_ms);
    int periodic_count = 0;
    int deferred_count = 0;
    int manual_ready_count = 0;
    int timed_ready_count = 0;

    const uint64_t periodic_id = feather.PeriodicTask(
        [&periodic_count]() { ++periodic_count; },
        10,
        10,
        1,
        FSSchedulerPeriodicTaskRepeatAllocationType::Absolute
    );
    const uint64_t deferred_id = feather.DeferredTask(
        [&deferred_count]() { ++deferred_count; },
        25,
        1
    );

    if (!feather.IsTaskEnabled(periodic_id)) {
        std::cerr << "periodic task should start enabled\n";
        return 1;
    }
    if (!feather.CancelTask(deferred_id)) {
        std::cerr << "failed to cancel deferred task\n";
        return 1;
    }
    if (feather.CancelTask(deferred_id)) {
        std::cerr << "cancelled deferred task twice\n";
        return 1;
    }
    if (!feather.SetTaskEnabled(periodic_id, false)) {
        std::cerr << "failed to disable periodic task\n";
        return 1;
    }
    if (feather.IsTaskEnabled(periodic_id)) {
        std::cerr << "periodic task still reported enabled\n";
        return 1;
    }

    for (fake_now_ms_value = 0; fake_now_ms_value <= 30; fake_now_ms_value += 5) {
        feather.step();
    }

    if (periodic_count != 0) {
        std::cerr << "disabled periodic task should not execute\n";
        return 1;
    }
    if (deferred_count != 0) {
        std::cerr << "cancelled deferred task should not execute\n";
        return 1;
    }
    if (!feather.SetTaskEnabled(periodic_id, true)) {
        std::cerr << "failed to re-enable periodic task\n";
        return 1;
    }
    if (!feather.IsTaskEnabled(periodic_id)) {
        std::cerr << "periodic task should report enabled after re-enable\n";
        return 1;
    }

    for (fake_now_ms_value = 35; fake_now_ms_value <= 70; fake_now_ms_value += 5) {
        feather.step();
    }

    if (periodic_count != 4) {
        std::cerr << "unexpected periodic execution count: " << periodic_count << "\n";
        return 1;
    }
    if (!feather.CancelTask(periodic_id)) {
        std::cerr << "failed to cancel periodic task\n";
        return 1;
    }
    if (feather.IsTaskEnabled(periodic_id)) {
        std::cerr << "cancelled periodic task should not be enabled\n";
        return 1;
    }
    if (feather.SetTaskEnabled(periodic_id, true)) {
        std::cerr << "cancelled periodic task should not be re-enabled\n";
        return 1;
    }

    fake_now_ms_value = 80;
    feather.step();

    if (periodic_count != 4) {
        std::cerr << "cancelled periodic task should not execute again\n";
        return 1;
    }

    struct ManualEventContext {
        bool pending = false;
        bool condition_met = false;
        int action_count = 0;
    };

    const auto manual_event_index = feather.events.add_event(
        FSEvent::make(
            ManualEventContext{},
            [](ManualEventContext& ctx, uint64_t) {
                return ctx.pending;
            },
            [](ManualEventContext& ctx, uint64_t) {
                return ctx.condition_met;
            },
            [&manual_ready_count](ManualEventContext& ctx, FSScheduler& scheduler, uint64_t) {
                ctx.pending = false;
                ctx.condition_met = false;
                ++ctx.action_count;
                scheduler.enqueue_ready_task(
                    [&manual_ready_count]() { ++manual_ready_count; },
                    1
                );
            }
        )
    );

    auto* manual_event = feather.events.event_at(manual_event_index);
    if (manual_event == nullptr) {
        std::cerr << "failed to retrieve manual event\n";
        return 1;
    }
    auto* manual_context = manual_event->context_as<ManualEventContext>();
    if (manual_context == nullptr) {
        std::cerr << "failed to retrieve manual event context\n";
        return 1;
    }

    manual_context->pending = true;
    if (feather.events.poll_all() != 0) {
        std::cerr << "manual event should not dispatch without condition\n";
        return 1;
    }
    if (manual_ready_count != 0) {
        std::cerr << "manual event should not enqueue work early\n";
        return 1;
    }

    manual_context->condition_met = true;
    if (feather.events.poll_all() != 1) {
        std::cerr << "manual event should dispatch once when condition matches\n";
        return 1;
    }
    if (!feather.scheduler.has_ready_tasks()) {
        std::cerr << "manual event should queue ready work\n";
        return 1;
    }
    if (manual_ready_count != 0) {
        std::cerr << "ready work should not execute until scheduler step\n";
        return 1;
    }

    feather.step();

    if (manual_ready_count != 1) {
        std::cerr << "manual event ready work did not execute\n";
        return 1;
    }
    if (manual_context->action_count != 1) {
        std::cerr << "manual event action count mismatch\n";
        return 1;
    }

    struct TimedEventContext {
        uint64_t next_check_ms = 0;
        int action_count = 0;
    };

    const auto timed_event_index = feather.events.add_event(
        FSEvent::make(
            TimedEventContext{95, 0},
            [](TimedEventContext& ctx, uint64_t now_ms) {
                return now_ms >= ctx.next_check_ms;
            },
            [](TimedEventContext&, uint64_t) {
                return true;
            },
            [&timed_ready_count](TimedEventContext& ctx, FSScheduler& scheduler, uint64_t now_ms) {
                ++ctx.action_count;
                ctx.next_check_ms = now_ms + 20;
                scheduler.enqueue_ready_task(
                    [&timed_ready_count]() { ++timed_ready_count; },
                    1
                );
            }
        )
    );

    auto* timed_event = feather.events.event_at(timed_event_index);
    if (timed_event == nullptr) {
        std::cerr << "failed to retrieve timed event\n";
        return 1;
    }
    auto* timed_context = timed_event->context_as<TimedEventContext>();
    if (timed_context == nullptr) {
        std::cerr << "failed to retrieve timed event context\n";
        return 1;
    }

    fake_now_ms_value = 90;
    if (feather.events.poll_all() != 0) {
        std::cerr << "timed event should not dispatch early\n";
        return 1;
    }

    fake_now_ms_value = 95;
    if (feather.events.poll_all() != 1) {
        std::cerr << "timed event should dispatch at its scheduled time\n";
        return 1;
    }
    if (timed_context->next_check_ms != 115) {
        std::cerr << "timed event should update its next check time\n";
        return 1;
    }
    if (timed_ready_count != 0) {
        std::cerr << "timed event work should stay queued until step\n";
        return 1;
    }

    feather.step();

    if (timed_ready_count != 1) {
        std::cerr << "timed event ready work did not execute\n";
        return 1;
    }
    fake_now_ms_value = 100;
    if (feather.events.poll_all() != 0) {
        std::cerr << "timed event should not re-dispatch before next check\n";
        return 1;
    }

    std::cout << "scheduler and event api ok\n";
    return 0;
}
