#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>

#include "FeatherRuntime/FSTime.hpp"
#include "FeatherRuntime/FSScheduler.hpp"
#include "FeatherRuntime/FSEvent.hpp"

// ---------------------------------------------------------------------------
// Feather – public API facade
//
// External interface uses "priority" (uint8_t, 0–15).
// Internally, the scheduler stores it as a 4-bit execution budget.
//
// All task-registration methods are templates so that lambdas (including
// those capturing local variables) can be passed directly at the call site
// without pre-declaring any static function or global state.
// ---------------------------------------------------------------------------
class Feather {
    FSTime clock;

public:
    FSScheduler scheduler;
    FSEvents events;
    uint64_t (*now_ms)();

    Feather()
        : clock([]() -> uint64_t { return 0; })
        , scheduler(clock)
        , events(&scheduler)
        , now_ms([]() -> uint64_t { return 0; }) {}

    explicit Feather(uint64_t (*current_time_ms_)())
        : clock(current_time_ms_)
        , scheduler(clock)
        , events(&scheduler)
        , now_ms(current_time_ms_) {}

    void step() {
        events.poll_all();
        scheduler.step();
    }

    bool CancelTask(uint64_t task_id) {
        return scheduler.cancel_task(task_id);
    }

    bool SetTaskEnabled(uint64_t task_id, bool enabled) {
        return scheduler.set_task_enabled(task_id, enabled);
    }

    bool IsTaskEnabled(uint64_t task_id) const {
        return scheduler.is_task_enabled(task_id);
    }

    template<typename F>
    uint64_t InstantTask(F&& task, uint8_t priority) {
        return scheduler.add_instant_task(std::forward<F>(task), priority);
    }

    template<typename F>
    uint64_t DeferredTask(F&& task, uint64_t time_to_run, uint8_t priority) {
        return scheduler.add_deferred_task(std::forward<F>(task), now_ms() + time_to_run, priority);
    }

    template<typename F>
    uint64_t PeriodicTask(
        F&&      task,
        uint64_t time_to_run,
        uint32_t repeat_cycle,
        uint8_t  priority,
        FSSchedulerPeriodicTaskRepeatAllocationType allocation_type =
            FSSchedulerPeriodicTaskRepeatAllocationType::Absolute
    ) {
        return scheduler.add_periodic_task(
            std::forward<F>(task), now_ms() + time_to_run, repeat_cycle, priority, allocation_type);
    }

    template<typename Condition, typename Task>
    FSEventHandle Event(
        Condition&& condition,
        Task&&      task,
        uint8_t     priority,
        bool        enabled = true
    ) {
        return events.add_event(
            FSEvent::make(
                std::forward<Condition>(condition),
                std::forward<Task>(task),
                priority,
                enabled
            )
        );
    }

    bool StartEvent(FSEventHandle event_id) {
        return events.start_event(event_id);
    }

    bool StopEvent(FSEventHandle event_id) {
        return events.stop_event(event_id);
    }

    bool DeleteEvent(FSEventHandle event_id) {
        return events.delete_event(event_id);
    }
};
