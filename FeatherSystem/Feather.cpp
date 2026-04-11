#include "Feather.hpp"
#include <cstdint>

Feather::Feather():
    clock([]() -> uint64_t { return 0; }),
    scheduler(clock) {
}
