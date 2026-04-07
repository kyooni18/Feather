# Feather

Feather is a lightweight cooperative task scheduler library written in C99.  
It provides priority queues, deferred (one-shot) tasks, and repeating tasks with fixed-delay or fixed-rate semantics — with no dynamic threading or OS dependencies.

## Features

- Three priority levels: Background, UI, Interactive
- Instant, deferred, and repeating task types
- Task cancellation and status queries via `Feather_cancel_task` / `Feather_task_status`
- Advanced scheduler controls via `feather.scheduler`: task handles, pause/resume/reschedule, deadline/timeout
- Pluggable time source (`FSTime` / custom `now_fn` callback)
- Pluggable allocator (`FSAllocator`)
- Optional `FeatherResourceTracking` extension for allocator-level memory accounting
- Arduino-compatible export
- C99, no external runtime dependencies

## Requirements

- **CMake** ≥ 3.20
- A **C99** compiler (GCC, Clang, MSVC)
- GNU Make (for convenience Makefile targets)

## Building

```sh
# Static library (default)
make

# Shared library
make shared
```

Build outputs are placed under `build/static/` and `build/shared/` respectively.

## Running the Tests

```sh
make check
```

This builds the static library, runs the built-in scheduler test suite (`Feather`), the smoke-test CLI (`FeatherTestCLI all`), and the resource-tracking tests (`FeatherResourceTrackingTest`).

### Additional diagnostics

For stress/performance validation (recommended after scheduler changes):

```sh
# Memory/resource stress diagnostics
./build/static/FeatherMemoryTest

# Extended functional + performance score
./build/static/FeatherSystemScoreTest

# Long-running mixed workload benchmark
./build/static/FeatherFullSystemBenchmark
```

These diagnostics are useful for catching queue/heap edge-case regressions (for example, growth/shrink behavior in ready queues and waiting heaps) that may not surface in basic API smoke tests.

## Installing

### Default prefix (`/usr/local`)

```sh
make install
```

### Custom prefix

```sh
make install prefix=/your/install/path
```

### Shared-library install

```sh
make install-shared prefix=/your/install/path
```

### What gets installed

| Artifact | Destination |
|---|---|
| `libFeather.a` / `.so` / `.dylib` | `<prefix>/lib/` |
| `libFeatherResourceTracking.*` (optional) | `<prefix>/lib/` |
| Public headers (`Feather.h`, `FeatherRuntime/`) | `<prefix>/include/` |
| CMake package files | `<prefix>/lib/cmake/Feather/` |
| `Feather.pc` (pkg-config, optional) | `<prefix>/lib/pkgconfig/` |

## Integrating with CMake

After installing, consume Feather from any CMake project:

```cmake
find_package(Feather REQUIRED)

target_link_libraries(my_app PRIVATE Feather::Feather)

# Optional: resource tracking extension
target_link_libraries(my_app PRIVATE Feather::FeatherResourceTracking)
```

If you installed to a non-standard prefix, set `CMAKE_PREFIX_PATH`:

```sh
cmake -DCMAKE_PREFIX_PATH=/your/install/path ..
```

## Integrating with pkg-config

```sh
pkg-config --cflags --libs Feather
```

Or in a Makefile:

```makefile
CFLAGS  += $(shell pkg-config --cflags Feather)
LDFLAGS += $(shell pkg-config --libs   Feather)
```

## Quick Usage

```c
#include "Feather.h"

static void my_task(void *ctx) { /* ... */ }

int main(void) {
    Feather feather;
    Feather_init(&feather);

    Feather_add_instant_task(&feather, (FSSchedulerInstantTask){
        .task     = my_task,
        .context  = NULL,
        .priority = FSScheduler_Priority_UI,
    });

    while (Feather_step(&feather)) { /* run until empty */ }

    Feather_deinit(&feather);
    return 0;
}
```

## Arduino Export

```sh
make arduino-export
```

Produces an Arduino-compatible library layout in `build/arduino/Feather/`, including `library.properties`.

## Documentation

Full module-level documentation:

- English: [`Docs/EN/README.md`](Docs/EN/README.md)
- 한국어: [`Docs/KR/README.md`](Docs/KR/README.md)
