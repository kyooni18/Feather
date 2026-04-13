#include <Feather.hpp>
#include <chrono>
#include <iostream>

uint64_t get_current_time_ms() {
	auto now = std::chrono::system_clock::now();
	auto duration = now.time_since_epoch();
	uint64_t ctime = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
	return ctime;
}


int main() {
	int count = 0;
	Feather feather(get_current_time_ms);

	for(int i = 0; i < 5000; i++) {
		feather.PeriodicTask([=, &count]() {
		count++;
		
	}, feather.now_ms(), 1, 1, FSSchedulerPeriodicTaskRepeatAllocationType::Absolute);
	}

	uint64_t now = get_current_time_ms();
	while(get_current_time_ms() - now < 5000) {
		feather.scheduler.step();
	}
	printf("Count: %d\n", count);
	return 0;
}