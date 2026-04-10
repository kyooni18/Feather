#include "FSTime.hpp"

FSTime::FSTime(std::uint64_t (*now_ms_func)()) {
    now_ms_f = now_ms_func;
}

std::uint64_t FSTime::now_ms() {
    return now_ms_f();
}