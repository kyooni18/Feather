# Module: Scheduler (`FSScheduler`)

## Purpose

`FSScheduler` is the core cooperative scheduler implementation.

Main files:
- `System/FeatherRuntime/FSScheduler.h`
- `System/FeatherRuntime/FSScheduler.c`

## Public Constants and Macros

- Capacity/ID:
  - `FSScheduler_TASK_CAPACITY`
  - `FSScheduler_TASK_INITIAL_CAPACITY`
  - `FSSchedulerTask_ID_SLOT_BITS`
  - `FSSchedulerTask_ID_SLOT_MASK`
- Priority values:
  - `FSScheduler_Priority_BACKGROUND`
  - `FSScheduler_Priority_UI`
  - `FSScheduler_Priority_INTERACTIVE`
- Fairness budgets:
  - `FSScheduler_BG_BUDGET`
  - `FSScheduler_UI_BUDGET`
  - `FSScheduler_INTERACTIVE_BUDGET`

## Public Types

- Task payloads:
  - `FSSchedulerInstantTask`
  - `FSSchedulerDeferredTask` (legacy spelling)
  - `FSSchedulerRepeatingTask`
- Enums:
  - `FSSchedulerTaskRepeatMode`
  - `FSSchedulerTaskKind`
  - `FSSchedulerTaskStatus`
- Runtime state/value types:
  - `FSSchedulerComponentMemorySnapshot`
  - `FSSchedulerStateSnapshot`
  - `FSSchedulerTaskHandler`
  - `FSScheduler`

`FSSchedulerTaskRecord` and `FSTaskQueue` are public in the header for ABI visibility, but are internal scheduler data-model types and not intended for external mutation.

## Exported init constants

- `FSSchedulerInstantTask_init`
- `FSSchedulerDeferredTask_init`
- `FSSchedulerRepeatingTask_init`
- `FSSchedulerTaskHandler_init`

These provide zero/default initializers for task structs.

## Complete Public Function Reference

### Core lifecycle
- `uint64_t FSScheduler_now_ms(void)`
- `void FSScheduler_init(FSScheduler *scheduler)`
- `void FSScheduler_init_with_allocator(FSScheduler *scheduler, const FSAllocator *allocator)`
- `void FSScheduler_deinit(FSScheduler *scheduler)`

### Task submission
- `uint64_t FSScheduler_add_instant_task(FSScheduler *scheduler, FSSchedulerInstantTask task)`
- `uint64_t FSScheduler_add_deferred_task(FSScheduler *scheduler, FSSchedulerDeferredTask task)`
- `uint64_t FSScheduler_add_repeating_task(FSScheduler *scheduler, FSSchedulerRepeatingTask task)`
- `FSSchedulerTaskHandler FSScheduler_add_instant_task_handle(FSScheduler *scheduler, FSSchedulerInstantTask task)`
- `FSSchedulerTaskHandler FSScheduler_add_deferred_task_handle(FSScheduler *scheduler, FSSchedulerDeferredTask task)`
- `FSSchedulerTaskHandler FSScheduler_add_repeating_task_handle(FSScheduler *scheduler, FSSchedulerRepeatingTask task)`

### Time source settings
- `bool FSScheduler_set_time_source(FSScheduler *scheduler, uint64_t (*now_fn)(void *context), void *context)`
- `bool FSScheduler_set_time_provider(FSScheduler *scheduler, const FSTime *provider)`

### Execution and sleeping
- `bool FSScheduler_step(FSScheduler *scheduler)`
- `bool FSScheduler_process_for_ms(FSScheduler *scheduler, uint64_t duration_ms)`
- `bool FSScheduler_has_pending_tasks(const FSScheduler *scheduler)`
- `bool FSScheduler_next_sleep_ms(const FSScheduler *scheduler, uint64_t *out_delay_ms)`

### Introspection/control
- `FSSchedulerComponentMemorySnapshot FSScheduler_component_memory_snapshot(const FSScheduler *scheduler)`
- `bool FSScheduler_cancel_task(FSScheduler *scheduler, uint64_t task_id)`
- `bool FSScheduler_pause_task(FSScheduler *scheduler, uint64_t task_id)`
- `bool FSScheduler_resume_task(FSScheduler *scheduler, uint64_t task_id)`
- `bool FSScheduler_reschedule_task(FSScheduler *scheduler, uint64_t task_id, uint64_t start_time_ms)`
- `bool FSScheduler_set_task_deadline(FSScheduler *scheduler, uint64_t task_id, uint64_t deadline_ms)`
- `bool FSScheduler_set_task_timeout(FSScheduler *scheduler, uint64_t task_id, uint64_t timeout_ms)`
- `FSSchedulerTaskStatus FSScheduler_task_status(const FSScheduler *scheduler, uint64_t task_id)`
- `FSSchedulerStateSnapshot FSScheduler_state_snapshot(const FSScheduler *scheduler)`
- `bool FSScheduler_task_handle_is_valid(const FSSchedulerTaskHandler *handle)`
- `bool FSScheduler_task_handle_cancel(FSScheduler *scheduler, FSSchedulerTaskHandler *handle)`
- `bool FSScheduler_task_handle_pause(FSScheduler *scheduler, const FSSchedulerTaskHandler *handle)`
- `bool FSScheduler_task_handle_resume(FSScheduler *scheduler, const FSSchedulerTaskHandler *handle)`
- `bool FSScheduler_task_handle_reschedule(FSScheduler *scheduler, const FSSchedulerTaskHandler *handle, uint64_t start_time_ms)`
- `bool FSScheduler_task_handle_set_deadline(FSScheduler *scheduler, const FSSchedulerTaskHandler *handle, uint64_t deadline_ms)`
- `bool FSScheduler_task_handle_set_timeout(FSScheduler *scheduler, const FSSchedulerTaskHandler *handle, uint64_t timeout_ms)`
- `FSSchedulerTaskStatus FSScheduler_task_handle_status(const FSScheduler *scheduler, const FSSchedulerTaskHandler *handle)`
- `uint64_t FSScheduler_task_handle_user_tag(const FSSchedulerTaskHandler *handle)`
- `bool FSScheduler_task_handle_set_user_tag(FSSchedulerTaskHandler *handle, uint64_t user_tag)`

## Usage Example

```c
#include "FeatherRuntime/FSScheduler.h"

static void run_count(void *ctx) {
    int *count = (int *)ctx;
    *count += 1;
}

int main(void) {
    FSScheduler scheduler;
    int count = 0;

    FSScheduler_init(&scheduler);

    (void)FSScheduler_add_repeating_task(&scheduler, (FSSchedulerRepeatingTask){
        .task = run_count,
        .context = &count,
        .start_time = 0,
        .execute_cycle = 16,
        .repeat_mode = FSSchedulerTaskRepeat_FIXEDRATE,
        .priority = FSScheduler_Priority_INTERACTIVE,
    });

    (void)FSScheduler_process_for_ms(&scheduler, 100);

    FSSchedulerStateSnapshot state = FSScheduler_state_snapshot(&scheduler);
    (void)state;

    FSScheduler_deinit(&scheduler);
    return 0;
}
```

## Behavioral Notes

- Repeating tasks require `execute_cycle > 0`.
- APIs return failure (`0`/`false`) on invalid inputs, capacity overflow, or allocation failures.
- Dispatch preference is Interactive → UI → Background, with fairness budgets to prevent starvation.
- Task payloads now support optional absolute `deadline` and `timeout` (`0` = disabled).
- For tasks of the same priority, earlier deadline runs first (both in waiting-heap tie-break and ready-queue dispatch).
- Timeout semantics: once `now > timeout`, the task is dropped and reported as `FSSchedulerTaskStatus_TIMED_OUT`.
- Pending states are now explicit: `PENDING_READY`, `PENDING_WAITING`, `PENDING_PAUSED`.
