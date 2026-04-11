#include "FeatherRuntime/FSTime.hpp"
#include "FeatherRuntime/FSScheduler.hpp"


class Feather {
    private:
    FSTime clock;
    FSScheduler scheduler;

    public:
    Feather();

    void InstantTask(void (*task_to_run)(...), uint8_t priority);
    void DefferedTask(void (*task_to_run)(...), uint64_t time_to_run, uint8_t priority);
    void PeriodicTask(void (*task_to_run)(...), uint64_t time_to_run, uint32_t repeat_cycle, uint8_t priority);
};