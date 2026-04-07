# Module: Core API (`Feather`)

## Purpose

`Feather` is the top-level entry point that wraps one `FSScheduler` instance and exposes a small user-facing API.

Main files:
- `System/Feather.h`
- `System/Feather.c`

## Public Types and Aliases

### `FeatherConfig`
Initialization config:
- `allocator`: optional custom allocator (`FSAllocator`)
- `now_fn`: optional custom monotonic clock callback
- `now_context`: context passed to `now_fn`
- `time_provider`: optional `FSTime` provider (`NULL` = platform default)

### `Feather`
Container type with one member:
- `FSScheduler scheduler`

### `FeatherComponentMemorySnapshot` (alias)
Alias of `FSSchedulerComponentMemorySnapshot`.

### Exported constant
- `FeatherConfig_init`: default-initialized config constant.

## Complete Public Function Reference

### Initialization and lifetime
- `bool Feather_init(Feather *feather)`
- `bool Feather_init_with_config(Feather *feather, const FeatherConfig *config)`
- `void Feather_deinit(Feather *feather)`

### Task submission
- `uint64_t Feather_add_instant_task(Feather *feather, FSSchedulerInstantTask task)`
- `uint64_t Feather_add_deferred_task(Feather *feather, FSSchedulerDeferredTask task)`
- `uint64_t Feather_add_repeating_task(Feather *feather, FSSchedulerRepeatingTask task)`

Returns assigned task ID on success, `0` on failure.

### Time configuration
- `bool Feather_set_time_source(Feather *feather, uint64_t (*now_fn)(void *context), void *context)`
- `bool Feather_set_time_provider(Feather *feather, const FSTime *provider)`

### Runtime execution
- `bool Feather_step(Feather *feather)`
- `bool Feather_process_for_ms(Feather *feather, uint64_t duration_ms)`
- `bool Feather_has_pending_tasks(const Feather *feather)`
- `bool Feather_next_sleep_ms(const Feather *feather, uint64_t *out_delay_ms)`

### Task control/inspection
- `bool Feather_cancel_task(Feather *feather, uint64_t task_id)`
- `FSSchedulerTaskStatus Feather_task_status(const Feather *feather, uint64_t task_id)`
- `FSSchedulerStateSnapshot Feather_state_snapshot(const Feather *feather)`
- `FeatherComponentMemorySnapshot Feather_component_memory_snapshot(const Feather *feather)`

## Usage Example

```c
#include "Feather.h"
#include <stdio.h>

static void print_task(void *ctx) {
    const char *label = (const char *)ctx;
    printf("task: %s\n", label);
}

int main(void) {
    Feather feather;
    if (!Feather_init(&feather)) {
        return 1;
    }

    uint64_t instant_id = Feather_add_instant_task(&feather, (FSSchedulerInstantTask){
        .task = print_task,
        .context = "instant",
        .priority = FSScheduler_Priority_UI,
    });

    uint64_t delayed_id = Feather_add_deferred_task(&feather, (FSSchedulerDeferredTask){
        .task = print_task,
        .context = "delayed",
        .start_time = FSScheduler_now_ms() + 100,
        .priority = FSScheduler_Priority_BACKGROUND,
    });

    if (instant_id != 0) {
        (void)Feather_cancel_task(&feather, instant_id);
    }

    while (Feather_has_pending_tasks(&feather)) {
        if (!Feather_step(&feather)) {
            uint64_t sleep_ms = 0;
            if (Feather_next_sleep_ms(&feather, &sleep_ms)) {
                FSTime_sleep_ms(sleep_ms);
            }
        }
    }

    (void)delayed_id;
    Feather_deinit(&feather);
    return 0;
}
```

## Notes

- `Feather` remains intentionally small; advanced runtime details are still available via `feather.scheduler`.
- `Feather_add_deferred_task` uses the public type name `FSSchedulerDeferredTask` (legacy spelling in API).
- Handle/pause/resume/reschedule/deadline/timeout APIs are intentionally exposed on `FSScheduler` (via `feather.scheduler`) rather than added to the small `Feather` facade.
- Additional task states now include `FSSchedulerTaskStatus_PENDING_PAUSED` and `FSSchedulerTaskStatus_TIMED_OUT`, and state snapshots include `paused_count`.
