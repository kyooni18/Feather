## Explanation

Feather is a utility Library, mainly for embedded environments.
Starts from single-threaded scheduler, expanding to GUI and so on.

## Currently available modules:
Feather keeps `Feather.hpp` as the top-level user API. Internally the public
headers and build targets are organized as these module boundaries:

- `Core`: clock abstraction. `FSTime` remains the v1 public type name and is
  available through `Core/Clock.hpp`.
- `Scheduler`: single-threaded cooperative scheduler. `FSScheduler` owns
  task/ready/timed/instant execution policy, one-step-per-step dispatch,
  priority budget execution, and `uint64_t` task IDs.
- `Events`: optional event layer above the scheduler. `FSEvent/FSEvents` poll
  from `Feather::step()` and dispatch matched work through the scheduler ready
  queue.
- `UI`: optional display helpers. These are not part of the core runtime
  dependency.
- `Platform`: reserved boundary for platform adapters.

Compatibility headers under `FeatherRuntime/` are still installed so existing
code can keep including the old paths.

### CMake targets

- `Feather::Core`
- `Feather::Scheduler`
- `Feather::Events`
- `Feather::Feather`
- `Feather::Shared`
- `Feather::UI`

## Simple usage:
``` C++
#include <Feather/Feather.hpp> // install with "sudo make install"

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

## Events module usage

`Feather.hpp` is scheduler-only. Events are opt-in.

```c++
#include <Feather/Feather.hpp>
#include <Feather/FeatherRuntime/FSEvents.hpp>

Feather feather(now_ms_fn);
FSEvents events(feather);

auto h = events.add_event(
  [](uint64_t now){ return (now % 1000) == 0; },
  [](){ /* event task */ },
  1
);

events.poll_all();      // manual polling
// or
// events.start_loop(1, 0); // periodic scheduler task loop
```
