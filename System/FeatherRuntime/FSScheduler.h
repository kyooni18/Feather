#ifndef FS_SCHEDULER_H
#define FS_SCHEDULER_H

#include "../FeatherExport.h"
#include "FSAllocator.h"
#include "FSTime.h"

#include <stdbool.h>
#include <stdint.h>

#define FSScheduler_TASK_CAPACITY 1024
#define FSScheduler_TASK_INITIAL_CAPACITY 16

/* Slot+generation encoding for task IDs.
 * Lower FSSchedulerTask_ID_SLOT_BITS bits hold the slot index (0..TASK_CAPACITY-1).
 * Upper (64-FSSchedulerTask_ID_SLOT_BITS) bits hold the generation counter.
 * SLOT_BITS must satisfy 2^SLOT_BITS >= FSScheduler_TASK_CAPACITY. */
#define FSSchedulerTask_ID_SLOT_BITS 10u
#define FSSchedulerTask_ID_SLOT_MASK ((UINT64_C(1) << FSSchedulerTask_ID_SLOT_BITS) - UINT64_C(1))
#define FSScheduler_Priority_BACKGROUND 0
#define FSScheduler_Priority_UI 1
#define FSScheduler_Priority_INTERACTIVE 2

#define FSScheduler_BG_BUDGET 1
#define FSScheduler_UI_BUDGET 2
#define FSScheduler_INTERACTIVE_BUDGET 4

typedef enum FSSchedulerTaskRepeatMode {
  FSSchedulerTaskRepeat_FIXEDDELAY = 0,
  FSSchedulerTaskRepeat_FIXEDRATE = 1
} FSSchedulerTaskRepeatMode;

struct FSScheduler;

/* A task that is executed as soon as it is scheduled. */
typedef struct FSSchedulerInstantTask {
  void (*task)(void *context); /* function pointer */
  void *context;               /* data pointer passed to task callback */
  uint64_t id;                      /* assigned by scheduler on enqueue (0 initially) */
  uint64_t deadline;          /* optional absolute deadline in monotonic ms (0 = none) */
  uint64_t timeout;           /* optional absolute timeout in monotonic ms (0 = none) */
  uint8_t priority;            /* FSScheduler_Priority_BACKGROUND/UI/INTERACTIVE */
} FSSchedulerInstantTask;

/* A task that is executed once after a specified absolute start time. */
typedef struct FSSchedulerDeferredTask {
  void (*task)(void *context); /* function pointer */
  void *context;               /* data pointer passed to task callback */
  uint64_t start_time;         /* earliest execution time in monotonic ms */
  uint64_t id;                      /* assigned by scheduler on enqueue (0 initially) */
  uint64_t deadline;          /* optional absolute deadline in monotonic ms (0 = none) */
  uint64_t timeout;           /* optional absolute timeout in monotonic ms (0 = none) */
  uint8_t priority;            /* FSScheduler_Priority_BACKGROUND/UI/INTERACTIVE */
} FSSchedulerDeferredTask;

/* A task that is executed repeatedly at a fixed interval. */
typedef struct FSSchedulerRepeatingTask {
  void (*task)(void *context); /* function pointer */
  void *context;               /* data pointer passed to task callback */
  uint64_t start_time;         /* first execution time in monotonic ms (0 = now) */
  uint64_t execute_cycle;      /* repeat interval in ms */
  uint64_t id;                      /* assigned by scheduler on enqueue (0 initially) */
  uint64_t deadline;          /* optional absolute deadline in monotonic ms (0 = none) */
  uint64_t timeout;           /* optional absolute timeout in monotonic ms (0 = none) */
  FSSchedulerTaskRepeatMode repeat_mode;
  uint8_t priority; /* FSScheduler_Priority_BACKGROUND/UI/INTERACTIVE */
} FSSchedulerRepeatingTask;

typedef enum FSSchedulerTaskKind {
  FSSchedulerTaskKind_INSTANT = 0,
  FSSchedulerTaskKind_DEFERRED = 1,
  FSSchedulerTaskKind_REPEATING = 2
} FSSchedulerTaskKind;

/* Internal task representation used by FSSchedulerTaskQueue and FSScheduler internals.
 * All public task fields are flattened into a single 64-byte struct so that
 * every access is a direct field read/write rather than a switch through a
 * union, and so that per-record memory footprint is minimised. */
typedef struct FSSchedulerTaskRecord {
  void     (*task)(void *context); /* callback */
  void      *context;
  uint64_t   id;
  uint64_t   start_time;    /* 0 for instant tasks */
  uint64_t   execute_cycle; /* 0 for non-repeating tasks */
  uint64_t   deadline;      /* optional absolute deadline in ms (0 = none) */
  uint64_t   timeout;       /* optional absolute timeout in ms (0 = none) */
  uint8_t    kind;          /* FSSchedulerTaskKind */
  uint8_t    priority;      /* FSScheduler_Priority_* */
  uint8_t    repeat_mode;   /* FSSchedulerTaskRepeatMode (0 for non-repeating) */
  uint8_t    _pad[5];
} FSSchedulerTaskRecord; /* 64 bytes */

typedef struct FSSchedulerTaskQueue {
  FSSchedulerTaskRecord *tasks;
  int count;
  int capacity;
  int head;
  int tail;
  int deadline_count;
} FSSchedulerTaskQueue;

typedef struct FSSchedulerComponentMemorySnapshot {
  size_t background_queue_bytes;
  size_t ui_queue_bytes;
  size_t interactive_queue_bytes;
  size_t waiting_heap_bytes;
  size_t total_bytes;
} FSSchedulerComponentMemorySnapshot;

typedef enum FSSchedulerTaskStatus {
  FSSchedulerTaskStatus_NOT_FOUND     = 0, /* task is not in the scheduler */
  FSSchedulerTaskStatus_PENDING_READY   = 1, /* task is in a ready queue */
  FSSchedulerTaskStatus_PENDING_WAITING = 2, /* task is in the waiting heap */
  FSSchedulerTaskStatus_PENDING_PAUSED = 3,  /* task is paused */
  FSSchedulerTaskStatus_TIMED_OUT = 4        /* task was dropped by timeout */
} FSSchedulerTaskStatus;

typedef struct FSSchedulerTaskHandler {
  uint64_t task_id;
  uint64_t user_tag;
} FSSchedulerTaskHandler;

typedef struct FSSchedulerStateSnapshot {
  int      background_ready_count;
  int      ui_ready_count;
  int      interactive_ready_count;
  int      waiting_count;
  int      paused_count;
  int      total_pending;
  bool     has_earliest_wake_time;
  uint64_t earliest_wake_time_ms;
} FSSchedulerStateSnapshot;

typedef struct FSScheduler {
  FSSchedulerTaskQueue bgReadyQueue;
  FSSchedulerTaskQueue uiReadyQueue;
  FSSchedulerTaskQueue interactiveReadyQueue;

  FSSchedulerTaskRecord *waitingTasks; /* Min-heap ordered by start_time */
  int waitingCount;
  int waitingCapacity;
  FSSchedulerTaskRecord *pausedTasks;
  int pausedCount;
  int pausedCapacity;

  int bgCount;
  int uiCount;
  int interactiveCount;

  bool has_earliest_wake_time;
  uint64_t earliest_wake_time;
  uint64_t earliestTimeout; /* min of all active task timeouts; 0 = no timeout tracked */

  /* Packed ready-cycle budgets:
     - bg: 1 bit (0..1)
     - ui: 2 bits (0..2)
     - interactive: 3 bits (0..4) */
  uint8_t budgetsPacked;

  uint64_t (*now_fn)(void *context);
  void *now_context;
  const FSTime *time_provider;  /* Platform time-provider; default is &FSTime_init. */
  FSAllocator allocator;
  uint32_t nextSlot;   /* Slot index; cycles 0..(TASK_CAPACITY-1) */
  uint64_t generation; /* Incremented each time nextSlot wraps around */
  /* One bit per slot: set when the task that occupied that slot timed out.
   * Cleared when a new task is assigned to the same slot.
   * 1024 slots / 64 bits per word = 16 words = 128 bytes (vs 8192 for uint64_t[1024]). */
  uint64_t timedOutBits[FSScheduler_TASK_CAPACITY / 64];
  uint8_t slotLocation[FSScheduler_TASK_CAPACITY];
  int16_t slotIndex[FSScheduler_TASK_CAPACITY];
} FSScheduler;

FEATHER_EXTERN_C_BEGIN

extern FEATHER_API const FSSchedulerInstantTask FSSchedulerInstantTask_init;
extern FEATHER_API const FSSchedulerDeferredTask FSSchedulerDeferredTask_init;
extern FEATHER_API const FSSchedulerRepeatingTask FSSchedulerRepeatingTask_init;
extern FEATHER_API const FSSchedulerTaskHandler FSSchedulerTaskHandler_init;

FEATHER_API uint64_t FSScheduler_now_ms(void);
FEATHER_API void FSScheduler_init(FSScheduler *scheduler);
FEATHER_API void FSScheduler_init_with_allocator(FSScheduler *scheduler,
                                                 const FSAllocator *allocator);
FEATHER_API void FSScheduler_deinit(FSScheduler *scheduler);
FEATHER_API uint64_t FSScheduler_add_instant_task(FSScheduler *scheduler,
                                                  FSSchedulerInstantTask task);
FEATHER_API uint64_t FSScheduler_add_deferred_task(FSScheduler *scheduler,
                                                   FSSchedulerDeferredTask task);
FEATHER_API uint64_t FSScheduler_add_repeating_task(
    FSScheduler *scheduler, FSSchedulerRepeatingTask task);
FEATHER_API FSSchedulerTaskHandler
FSScheduler_add_instant_task_handle(FSScheduler *scheduler,
                                    FSSchedulerInstantTask task);
FEATHER_API FSSchedulerTaskHandler
FSScheduler_add_deferred_task_handle(FSScheduler *scheduler,
                                     FSSchedulerDeferredTask task);
FEATHER_API FSSchedulerTaskHandler
FSScheduler_add_repeating_task_handle(FSScheduler *scheduler,
                                      FSSchedulerRepeatingTask task);
FEATHER_API bool FSScheduler_set_time_source(FSScheduler *scheduler,
                                             uint64_t (*now_fn)(void *context),
                                             void *context);
FEATHER_API bool FSScheduler_set_time_provider(FSScheduler *scheduler,
                                               const FSTime *provider);
FEATHER_API bool FSScheduler_has_pending_tasks(const FSScheduler *scheduler);
FEATHER_API bool FSScheduler_next_sleep_ms(const FSScheduler *scheduler,
                                           uint64_t *out_delay_ms);
FEATHER_API bool FSScheduler_process_for_ms(FSScheduler *scheduler,
                                             uint64_t duration_ms);
FEATHER_API bool FSScheduler_step(FSScheduler *scheduler);
FEATHER_API FSSchedulerComponentMemorySnapshot
FSScheduler_component_memory_snapshot(const FSScheduler *scheduler);
FEATHER_API bool FSScheduler_cancel_task(FSScheduler *scheduler,
                                         uint64_t task_id);
FEATHER_API bool FSScheduler_pause_task(FSScheduler *scheduler,
                                        uint64_t task_id);
FEATHER_API bool FSScheduler_resume_task(FSScheduler *scheduler,
                                         uint64_t task_id);
FEATHER_API bool FSScheduler_reschedule_task(FSScheduler *scheduler,
                                             uint64_t task_id,
                                             uint64_t start_time_ms);
FEATHER_API bool FSScheduler_set_task_deadline(FSScheduler *scheduler,
                                               uint64_t task_id,
                                               uint64_t deadline_ms);
FEATHER_API bool FSScheduler_set_task_timeout(FSScheduler *scheduler,
                                              uint64_t task_id,
                                              uint64_t timeout_ms);
FEATHER_API FSSchedulerTaskStatus
FSScheduler_task_status(const FSScheduler *scheduler, uint64_t task_id);
FEATHER_API FSSchedulerStateSnapshot
FSScheduler_state_snapshot(const FSScheduler *scheduler);
FEATHER_API bool
FSScheduler_task_handle_is_valid(const FSSchedulerTaskHandler *handle);
FEATHER_API uint64_t
FSScheduler_task_handle_user_tag(const FSSchedulerTaskHandler *handle);
FEATHER_API bool
FSScheduler_task_handle_set_user_tag(FSSchedulerTaskHandler *handle,
                                     uint64_t user_tag);
FEATHER_API bool FSScheduler_task_handle_cancel(FSScheduler *scheduler,
                                                FSSchedulerTaskHandler *handle);
FEATHER_API bool FSScheduler_task_handle_pause(FSScheduler *scheduler,
                                               const FSSchedulerTaskHandler *handle);
FEATHER_API bool FSScheduler_task_handle_resume(FSScheduler *scheduler,
                                                const FSSchedulerTaskHandler *handle);
FEATHER_API bool FSScheduler_task_handle_reschedule(
    FSScheduler *scheduler, const FSSchedulerTaskHandler *handle,
    uint64_t start_time_ms);
FEATHER_API bool FSScheduler_task_handle_set_deadline(
    FSScheduler *scheduler, const FSSchedulerTaskHandler *handle,
    uint64_t deadline_ms);
FEATHER_API bool FSScheduler_task_handle_set_timeout(
    FSScheduler *scheduler, const FSSchedulerTaskHandler *handle,
    uint64_t timeout_ms);
FEATHER_API FSSchedulerTaskStatus FSScheduler_task_handle_status(
    const FSScheduler *scheduler, const FSSchedulerTaskHandler *handle);

FEATHER_EXTERN_C_END

#endif
