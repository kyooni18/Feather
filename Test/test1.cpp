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
    int manual_task_count = 0;
    int timed_ready_count = 0;
    int timed_task_count = 0;
    uint64_t timed_event_next_check_ms = 95;

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

    {
        FSTime ready_clock(fake_now_ms);
        FSScheduler ready_scheduler(ready_clock);
        int ready_order = 0;
        int low_ready_order = 0;
        int high_ready_order = 0;

        ready_scheduler.enqueue_ready_task(
            [&low_ready_order, &ready_order]() { low_ready_order = ++ready_order; },
            0
        );
        ready_scheduler.enqueue_ready_task(
            [&high_ready_order, &ready_order]() { high_ready_order = ++ready_order; },
            3
        );

        ready_scheduler.step();
        if (high_ready_order != 1 || low_ready_order != 0) {
            std::cerr << "ready queue should execute higher budget first\n";
            return 1;
        }
        ready_scheduler.step();
        if (low_ready_order != 2) {
            std::cerr << "ready queue should preserve lower budget work\n";
            return 1;
        }
    }

    {
        Feather budget_feather(fake_now_ms);
        int low_budget_count = 0;
        int high_budget_count = 0;

        budget_feather.InstantTask([&low_budget_count]() { ++low_budget_count; }, 0);
        budget_feather.InstantTask([&high_budget_count]() { ++high_budget_count; }, 3);

        for (int i = 0; i < 10; ++i) {
            budget_feather.step();
        }

        if (low_budget_count != 2 || high_budget_count != 8) {
            std::cerr << "instant tasks should use weighted budget slices: low="
                      << low_budget_count << " high=" << high_budget_count << "\n";
            return 1;
        }
    }

    {
        FSTime timed_clock(fake_now_ms);
        FSScheduler timed_scheduler(timed_clock);
        int timed_order = 0;
        int low_timed_order = 0;
        int high_timed_order = 0;

        fake_now_ms_value = 200;
        timed_scheduler.add_deferred_task(
            [&low_timed_order, &timed_order]() { low_timed_order = ++timed_order; },
            200,
            0
        );
        timed_scheduler.add_deferred_task(
            [&high_timed_order, &timed_order]() { high_timed_order = ++timed_order; },
            200,
            3
        );

        timed_scheduler.step();
        if (high_timed_order != 1 || low_timed_order != 2) {
            std::cerr << "timed tasks with matching fire time should use budget order\n";
            return 1;
        }
    }

    bool manual_pending = false;
    bool manual_condition_met = false;

    const auto manual_event_index = feather.Event(
        [&manual_pending, &manual_condition_met](uint64_t) {
            return manual_pending && manual_condition_met;
        },
        [&manual_pending, &manual_condition_met, &manual_task_count, &manual_ready_count]() {
            manual_pending = false;
            manual_condition_met = false;
            ++manual_task_count;
            ++manual_ready_count;
        },
        1
    );

    if (!feather.StopEvent(manual_event_index)) {
        std::cerr << "failed to stop manual event\n";
        return 1;
    }
    if (!feather.StartEvent(manual_event_index)) {
        std::cerr << "failed to start manual event\n";
        return 1;
    }

    manual_pending = true;
    feather.step();
    if (manual_task_count != 0) {
        std::cerr << "manual event should not dispatch without condition\n";
        return 1;
    }
    if (manual_ready_count != 0) {
        std::cerr << "manual event should not enqueue work early\n";
        return 1;
    }

    manual_condition_met = true;
    feather.step();
    if (manual_task_count != 1) {
        std::cerr << "manual event should dispatch once when condition matches\n";
        return 1;
    }
    if (manual_ready_count != 1) {
        std::cerr << "manual event ready work should execute from step\n";
        return 1;
    }
    if (manual_task_count != 1) {
        std::cerr << "manual event task count mismatch\n";
        return 1;
    }

    const auto timed_event_index = feather.Event(
        [&timed_event_next_check_ms](uint64_t now_ms) {
            return now_ms >= timed_event_next_check_ms;
        },
        [&timed_task_count, &timed_event_next_check_ms, &timed_ready_count](uint64_t now_ms) {
            ++timed_task_count;
            timed_event_next_check_ms = now_ms + 20;
            ++timed_ready_count;
        },
        1
    );

    if (!feather.StopEvent(timed_event_index)) {
        std::cerr << "failed to stop timed event\n";
        return 1;
    }

    fake_now_ms_value = 95;
    feather.step();
    if (timed_task_count != 0) {
        std::cerr << "stopped timed event should not dispatch\n";
        return 1;
    }

    if (!feather.StartEvent(timed_event_index)) {
        std::cerr << "failed to start timed event\n";
        return 1;
    }

    fake_now_ms_value = 90;
    feather.step();
    if (timed_task_count != 0) {
        std::cerr << "timed event should not dispatch early\n";
        return 1;
    }

    fake_now_ms_value = 95;
    feather.step();
    if (timed_task_count != 1) {
        std::cerr << "timed event should dispatch at its scheduled time\n";
        return 1;
    }
    if (timed_event_next_check_ms != 115) {
        std::cerr << "timed event should update its next check time\n";
        return 1;
    }
    if (timed_ready_count != 1) {
        std::cerr << "timed event ready work should execute from step\n";
        return 1;
    }

    fake_now_ms_value = 100;
    feather.step();
    if (timed_task_count != 1) {
        std::cerr << "timed event should not re-dispatch before next check\n";
        return 1;
    }

    manual_pending = true;
    manual_condition_met = true;
    if (!feather.DeleteEvent(manual_event_index)) {
        std::cerr << "failed to delete manual event\n";
        return 1;
    }
    if (feather.DeleteEvent(manual_event_index)) {
        std::cerr << "deleted manual event should not be deleted twice\n";
        return 1;
    }
    feather.step();
    if (manual_task_count != 1) {
        std::cerr << "deleted manual event should not dispatch\n";
        return 1;
    }
    if (feather.StartEvent(manual_event_index)) {
        std::cerr << "deleted manual event should not be restartable\n";
        return 1;
    }

    std::cout << "scheduler and event api ok\n";
    return 0;
}
