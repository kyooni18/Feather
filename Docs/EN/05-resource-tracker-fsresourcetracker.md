# Module: Resource Tracking (`FSResourceTracker`)

## Purpose

`FSResourceTracker` is an optional extension that wraps allocation calls and exposes memory/leak diagnostics.

Main files:
- `System/FeatherRuntime/FSResourceTracker.h`
- `System/FeatherRuntime/FSResourceTracker.cpp`

## Public Types

- `FSResourceTrackerRecord`
- `FSResourceTrackerSnapshot`
- `FSResourceTrackerConfig`
- `FSResourceTracker`
- `FSResourceTrackerSchedulerSnapshot`

## Exported constant

- `FSResourceTrackerConfig_init`: default tracker config.

## Complete Public Function Reference

### Lifecycle
- `bool FSResourceTracker_init(FSResourceTracker *tracking)`
- `bool FSResourceTracker_init_with_config(FSResourceTracker *tracking, const FSResourceTrackerConfig *config)`
- `void FSResourceTracker_deinit(FSResourceTracker *tracking)`

### Allocator bridge
- `const FSAllocator *FSResourceTracker_allocator(const FSResourceTracker *tracking)`

### Diagnostics
- `FSResourceTrackerSnapshot FSResourceTracker_snapshot(const FSResourceTracker *tracking)`
- `size_t FSResourceTracker_copy_active_records(const FSResourceTracker *tracking, FSResourceTrackerRecord *out_records, size_t max_records)`
- `bool FSResourceTracker_has_leaks(const FSResourceTracker *tracking)`
- `FSResourceTrackerSchedulerSnapshot FSResourceTracker_scheduler_snapshot(const FSResourceTracker *tracking, const FSScheduler *scheduler)`

## Usage Example

```c
#include "Feather.h"
#include "FeatherRuntime/FSResourceTracker.h"

static void noop_task(void *ctx) {
    (void)ctx;
}

int main(void) {
    FSResourceTracker tracker;
    Feather feather;

    if (!FSResourceTracker_init(&tracker)) {
        return 1;
    }

    FeatherConfig cfg = FeatherConfig_init;
    cfg.allocator = FSResourceTracker_allocator(&tracker);

    if (!Feather_init_with_config(&feather, &cfg)) {
        FSResourceTracker_deinit(&tracker);
        return 1;
    }

    (void)Feather_add_instant_task(&feather, (FSSchedulerInstantTask){
        .task = noop_task,
        .context = NULL,
        .priority = FSScheduler_Priority_BACKGROUND,
    });

    Feather_deinit(&feather);

    FSResourceTrackerSnapshot snap = FSResourceTracker_snapshot(&tracker);
    (void)snap;
    (void)FSResourceTracker_has_leaks(&tracker);

    FSResourceTracker_deinit(&tracker);
    return 0;
}
```

## Integration Notes

- Build and link `FeatherResourceTracking` in addition to `Feather`.
- Inject tracker allocator via `FeatherConfig.allocator`.
- Combined scheduler+memory visibility is available through `FSResourceTracker_scheduler_snapshot`.
