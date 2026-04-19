## Explanation

Feather is a utility Library, mainly for embedded environments.
Starts from single-threaded scheduler, expanding to GUI and so on.

## Currently available modules:
FSTime: Contains wrapper of time/clock functions. Used to track time flow with universal method.
- now_ms must be uint64_t function.

FSScheduler: single-threaded, cooperative scheduler.
- Supports running tasks needed to be executed at certain time, or executed regularly.
- Budget/cycle system based on tasks' priority. (0~15, packed into 8bit int)
- Budget controls runnable selection frequency (higher budget gets more execution opportunities per round).
- Individual IDs for Tasks. (uint64_t)
- Does not engross entire running loop, being executed only if step() is called.

## Simple usage:
``` C++
#include <Feather.h> // install with "sudo make install"

// ... other codes..

Feather feather([]() { return uint64_t(millis()); }); // Initializes Feather with wrapper function of millis()

int count = 0;

feather.InstantTask([]() {
		std::cout << "Feather Initialized.\n";
	}, // task
	1, // priority
);

feather.DeferredTask([]()  {
		std::cout << "loop started\n";
	}, // task
	1000, // executes 1000 milliseconds after start
	1 // priority
);

feather.PeriodicTask([=, &count]() {
		count++;
	}, //task,
	1000, // starts to be executed 1000 milliseconds after start
	1000, // being executed with interval of 1000 milliseconds
	1, // priority
	FSSchedulerPeriodicTaskRepeatAllocationType::Absolute // being executed with exactly 1000 ms of interval even if task execution is delayed
);

while(count < 5) {
	feather.step(); // runs one of its tasks
}

std::cout << "count: " << count << std::endl;

```

## Future plans

- some GUI stuffs for little more higher performance MCUs
