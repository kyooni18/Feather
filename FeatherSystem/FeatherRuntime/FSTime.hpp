#pragma once

#include <cstdint>

class FSTime {
    std::uint64_t (*now_ms_f)();

    public:

    FSTime(std::uint64_t (*now_ms_func)());
    std::uint64_t now_ms();
};