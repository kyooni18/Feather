#pragma once

#include "FeatherRuntime/FSTime.hpp"
#include "FeatherRuntime/FSScheduler.hpp"

// ---------------------------------------------------------------------------
// Feather – public API facade
//
// External interface uses "priority" (uint8_t, 0–15).
// Internally, the scheduler stores it as "budget".
//
// All task-registration methods are templates so that lambdas (including
// those capturing local variables) can be passed directly at the call site
// without pre-declaring any static function or global state.
// ---------------------------------------------------------------------------
class Feather {
    FSTime clock;

public:
    FSScheduler scheduler;
    uint64_t (*now_ms)();

    Feather()
        : clock([]() -> uint64_t { return 0; })
        , scheduler(clock)
        , now_ms([]() -> uint64_t { return 0; }) {}

    explicit Feather(uint64_t (*current_time_ms_)())
        : clock(current_time_ms_)
        , scheduler(clock)
        , now_ms(current_time_ms_) {}

    void step() {
        scheduler.step();
    }

    template<typename F>
    uint64_t InstantTask(F&& task, uint8_t priority) {
        return scheduler.add_instant_task(std::forward<F>(task), priority);
    }

    template<typename F>
    uint64_t DeferredTask(F&& task, uint64_t time_to_run, uint8_t priority) {
        return scheduler.add_deferred_task(std::forward<F>(task), time_to_run, priority);
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
            std::forward<F>(task), time_to_run, repeat_cycle, priority, allocation_type);
    }
};