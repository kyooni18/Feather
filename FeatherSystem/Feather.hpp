#include "FeatherRuntime/FSTime.hpp"
#include "FeatherRuntime/FSScheduler.hpp"

class Feather {
    FSTime clock;
    FSScheduler scheduler;

    Feather(void (now_ms_func*)());
};