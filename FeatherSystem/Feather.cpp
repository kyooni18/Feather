#include "Feather.hpp"
#include <cstdint>
#include <functional>

Feather::Feather():
    clock([]() -> uint64_t { return 0; }),
    scheduler(clock),
    now_ms([]() -> uint64_t { return 0; }) {}

Feather::Feather(uint64_t (*current_time_ms_)())
    : clock(current_time_ms_),
      scheduler(clock),
      now_ms(current_time_ms_) {}

void Feather::InstantTask(std::function<void()> task_to_run, uint8_t priority) {
    scheduler.add_instant_task(task_to_run, priority);
}
void Feather::DefferedTask(std::function<void()> task_to_run, uint64_t time_to_run, uint8_t priority) {
    scheduler.add_deferred_task(task_to_run, time_to_run, priority);
}
void Feather::PeriodicTask(std::function<void()> task_to_run, uint64_t time_to_run, uint32_t repeat_cycle, uint8_t priority, FSSchedulerPeriodicTaskRepeatAllocationType allocation_type) {
    scheduler.add_periodic_task(task_to_run, time_to_run, repeat_cycle, priority, allocation_type);
}
