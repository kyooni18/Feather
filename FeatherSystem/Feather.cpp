#include "Feather.hpp"
#include <cstdint>

Feather::Feather():
    clock([]() -> uint64_t { return 0; }),
    scheduler(clock) {
}

void Feather::InstantTask(void (*task_to_run)(...), uint8_t priority) {
    scheduler.add_instant_task(task_to_run, priority);
}
void Feather::DefferedTask(void (*task_to_run)(...), uint64_t time_to_run, uint8_t priority) {
    scheduler.add_deferred_task(task_to_run, time_to_run, priority);
}
void Feather::PeriodicTask(void (*task_to_run)(...), uint64_t time_to_run, uint32_t repeat_cycle, uint8_t priority) {
    scheduler.add_periodic_task(task_to_run, time_to_run, repeat_cycle, priority);
}