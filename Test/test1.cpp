#include <Feather.hpp>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <iostream>

uint64_t current_time() {
    const auto now = std::chrono::system_clock::now();
    time_t tm_now = std::chrono::system_clock::to_time_t(now);
    return static_cast<uint64_t>(tm_now);
}

void print_hi() {
    std::cout << "hi";
}

int main() {
    Feather feather(&current_time);
    feather.InstantTask(reinterpret_cast<void (*)(...)>(print_hi), 0);

    return 0;
}
