# Feather Documentation (EN)

This directory contains detailed module-by-module documentation for Feather.

## Modules

1. [Core API (`Feather`)](./01-core-feather.md)
2. [Scheduler (`FSScheduler`)](./02-scheduler-fsscheduler.md)
3. [Time Layer (`FSTime`)](./03-time-fstime.md)
4. [Allocator Abstraction (`FSAllocator`)](./04-allocator-fsallocator.md)
5. [Resource Tracking (`FSResourceTracker`)](./05-resource-tracker-fsresourcetracker.md)
6. [Build, Test, and Packaging](./06-build-test-packaging.md)
7. [Export/ABI Macros (`FeatherExport`)](./07-export-featherexport.md)

## Coverage Guide

- **All public modules**: `Feather`, `FSScheduler`, `FSTime`, `FSAllocator`, `FSResourceTracker`, build/packaging, and export/ABI macros.
- **All public functions**: every `FEATHER_API` function from public headers is listed in the module documents.
- **Aliases and exported constants**: type aliases (for example `FeatherComponentMemorySnapshot`) and exported `*_init` / system constants are documented.
- **Usage examples**: each runtime module includes at least one practical C usage example.

## Header-to-Document Map

| Public header | Document |
|---|---|
| `System/Feather.h` | [01-core-feather.md](./01-core-feather.md) |
| `System/FeatherRuntime/FSScheduler.h` | [02-scheduler-fsscheduler.md](./02-scheduler-fsscheduler.md) |
| `System/FeatherRuntime/FSTime.h` | [03-time-fstime.md](./03-time-fstime.md) |
| `System/FeatherRuntime/FSAllocator.h` | [04-allocator-fsallocator.md](./04-allocator-fsallocator.md) |
| `System/FeatherRuntime/FSResourceTracker.h` | [05-resource-tracker-fsresourcetracker.md](./05-resource-tracker-fsresourcetracker.md) |
| `System/FeatherExport.h` | [07-export-featherexport.md](./07-export-featherexport.md) |
| `Makefile`, `CMakeLists.txt` | [06-build-test-packaging.md](./06-build-test-packaging.md) |
