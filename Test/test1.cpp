#include <Feather/Feather.hpp>
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
    
    size_t featherSize = sizeof(feather);
    size_t scsSize = sizeof(feather.scheduler);
    feather.InstantTask(reinterpret_cast<void (*)(...)>(print_hi), 0);
    feather.InstantTask(reinterpret_cast<void (*)(...)>(print_hi), 0);
    feather.InstantTask(reinterpret_cast<void (*)(...)>(print_hi), 0);
    feather.InstantTask(reinterpret_cast<void (*)(...)>(print_hi), 0);
    feather.InstantTask(reinterpret_cast<void (*)(...)>(print_hi), 0);
    feather.InstantTask(reinterpret_cast<void (*)(...)>(print_hi), 0);

    size_t feathe2Size = sizeof(feather);
    size_t sceSize = sizeof(feather.scheduler);

    std::cout << "Init Feather Size: " << featherSize << '\n';
    std::cout << "Init Scheduler Size: " << scsSize << '\n';
    std::cout << "After Feather Size: " << feathe2Size << '\n';
    std::cout << "After Scheduler Size: " << sceSize << '\n';
    
    return 0;
}
