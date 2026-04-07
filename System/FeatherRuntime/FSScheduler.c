#include "FSScheduler.h"

#include <stdlib.h>
#include <string.h>

#include "FSTime.h"

const FSSchedulerInstantTask FSSchedulerInstantTask_init = {
    .id = 0,
    .task = NULL,
    .context = NULL,
    .deadline = 0,
    .timeout = 0,
    .priority = 0};

const FSSchedulerDeferredTask FSSchedulerDeferredTask_init = {
    .id = 0,
    .task = NULL,
    .context = NULL,
    .start_time = 0,
    .deadline = 0,
    .timeout = 0,
    .priority = 0};

const FSSchedulerRepeatingTask FSSchedulerRepeatingTask_init = {
    .id = 0,
    .task = NULL,
    .context = NULL,
    .start_time = 0,
    .execute_cycle = 0,
    .deadline = 0,
    .timeout = 0,
    .repeat_mode = FSSchedulerTaskRepeat_FIXEDDELAY,
    .priority = 0};

const FSSchedulerTaskHandler FSSchedulerTaskHandler_init = {.task_id = 0,
                                                          .user_tag = 0};

static void (*FSSchedulerTaskRecord_callback(const FSSchedulerTaskRecord *task))(
    void *context) {
  return (task != NULL) ? task->task : NULL;
}

static void *FSSchedulerTaskRecord_context(const FSSchedulerTaskRecord *task) {
  return (task != NULL) ? task->context : NULL;
}

static uint8_t FSSchedulerTaskRecord_priority(const FSSchedulerTaskRecord *task) {
  return (task != NULL) ? task->priority : 0;
}

static uint64_t FSSchedulerTaskRecord_start_time(
    const FSSchedulerTaskRecord *task) {
  return (task != NULL) ? task->start_time : 0;
}

static uint64_t FSSchedulerTaskRecord_deadline(
    const FSSchedulerTaskRecord *task) {
  return (task != NULL) ? task->deadline : 0;
}

static uint64_t FSSchedulerTaskRecord_timeout(
    const FSSchedulerTaskRecord *task) {
  return (task != NULL) ? task->timeout : 0;
}

static void FSSchedulerTaskRecord_set_start_time(FSSchedulerTaskRecord *task,
                                                 uint64_t start_time) {
  if (task != NULL) {
    task->start_time = start_time;
  }
}

static void FSSchedulerTaskRecord_set_deadline(FSSchedulerTaskRecord *task,
                                               uint64_t deadline) {
  if (task != NULL) {
    task->deadline = deadline;
  }
}

static void FSSchedulerTaskRecord_set_timeout(FSSchedulerTaskRecord *task,
                                              uint64_t timeout) {
  if (task != NULL) {
    task->timeout = timeout;
  }
}

static bool FSSchedulerTaskRecord_is_repeating(
    const FSSchedulerTaskRecord *task) {
  return task != NULL && task->kind == FSSchedulerTaskKind_REPEATING;
}

static uint64_t FSSchedulerTaskRecord_execute_cycle(
    const FSSchedulerTaskRecord *task) {
  return FSSchedulerTaskRecord_is_repeating(task) ? task->execute_cycle : 0;
}

static FSSchedulerTaskRepeatMode FSSchedulerTaskRecord_repeat_mode(
    const FSSchedulerTaskRecord *task) {
  return FSSchedulerTaskRecord_is_repeating(task)
             ? (FSSchedulerTaskRepeatMode)task->repeat_mode
             : FSSchedulerTaskRepeat_FIXEDDELAY;
}

static uint64_t FSSchedulerTaskRecord_get_id(
    const FSSchedulerTaskRecord *task) {
  return (task != NULL) ? task->id : 0;
}

uint64_t FSScheduler_now_ms(void) { return FSTime_init.now_monotonic_ms(); }

static const FSAllocator *FSScheduler_allocator(
    const FSScheduler *scheduler) {
  if (scheduler == NULL) {
    return FSAllocator_resolve(NULL);
  }

  return FSAllocator_resolve(&scheduler->allocator);
}

static uint64_t FSScheduler_default_now_fn(void *context) {
  const FSScheduler *scheduler = (const FSScheduler *)context;
  if (scheduler == NULL || scheduler->time_provider == NULL ||
      scheduler->time_provider->now_monotonic_ms == NULL) {
    return FSScheduler_now_ms();
  }
  return scheduler->time_provider->now_monotonic_ms();
}

static uint64_t FSScheduler_current_time_ms(const FSScheduler *scheduler) {
  if (scheduler == NULL || scheduler->now_fn == NULL) {
    return FSScheduler_now_ms();
  }

  return scheduler->now_fn(scheduler->now_context);
}

static bool FSScheduler_grow_task_buffer(FSSchedulerTaskRecord **tasks,
                                         int *capacity,
                                         const FSAllocator *allocator);

static bool FSScheduler_assign_task_id(FSScheduler *scheduler, FSSchedulerTaskRecord *task) {
  if (scheduler == NULL || task == NULL) {
    return false;
  }

  /* Pre-increment the slot so the first assigned ID has slot=1 (never 0). */
  scheduler->nextSlot++;
  if (scheduler->nextSlot >= FSScheduler_TASK_CAPACITY) {
    scheduler->nextSlot = 0;
    scheduler->generation++;
  }

  task->id = ((uint64_t)scheduler->generation << FSSchedulerTask_ID_SLOT_BITS) |
             (uint64_t)scheduler->nextSlot;
  return true;
}

enum {
  FSScheduler_BG_BUDGET_SHIFT = 0,
  FSScheduler_UI_BUDGET_SHIFT = 1,
  FSScheduler_INTERACTIVE_BUDGET_SHIFT = 3
};

enum {
  FSScheduler_BG_BUDGET_MASK = 0x01u,
  FSScheduler_UI_BUDGET_MASK = 0x03u,
  FSScheduler_INTERACTIVE_BUDGET_MASK = 0x07u
};

static int FSScheduler_get_bg_budget(const FSScheduler *scheduler) {
  return (int)((scheduler->budgetsPacked >> FSScheduler_BG_BUDGET_SHIFT) &
               FSScheduler_BG_BUDGET_MASK);
}

static int FSScheduler_get_ui_budget(const FSScheduler *scheduler) {
  return (int)((scheduler->budgetsPacked >> FSScheduler_UI_BUDGET_SHIFT) &
               FSScheduler_UI_BUDGET_MASK);
}

static int FSScheduler_get_interactive_budget(const FSScheduler *scheduler) {
  return (
      int)((scheduler->budgetsPacked >> FSScheduler_INTERACTIVE_BUDGET_SHIFT) &
           FSScheduler_INTERACTIVE_BUDGET_MASK);
}

static void FSScheduler_set_bg_budget(FSScheduler *scheduler, int budget) {
  scheduler->budgetsPacked =
      (uint8_t)((scheduler->budgetsPacked &
                 (uint8_t)~(FSScheduler_BG_BUDGET_MASK
                            << FSScheduler_BG_BUDGET_SHIFT)) |
                ((uint8_t)(budget & (int)FSScheduler_BG_BUDGET_MASK)
                 << FSScheduler_BG_BUDGET_SHIFT));
}

static void FSScheduler_set_ui_budget(FSScheduler *scheduler, int budget) {
  scheduler->budgetsPacked =
      (uint8_t)((scheduler->budgetsPacked &
                 (uint8_t)~(FSScheduler_UI_BUDGET_MASK
                            << FSScheduler_UI_BUDGET_SHIFT)) |
                ((uint8_t)(budget & (int)FSScheduler_UI_BUDGET_MASK)
                 << FSScheduler_UI_BUDGET_SHIFT));
}

static void FSScheduler_set_interactive_budget(FSScheduler *scheduler,
                                               int budget) {
  scheduler->budgetsPacked =
      (uint8_t)((scheduler->budgetsPacked &
                 (uint8_t)~(FSScheduler_INTERACTIVE_BUDGET_MASK
                            << FSScheduler_INTERACTIVE_BUDGET_SHIFT)) |
                ((uint8_t)(budget & (int)FSScheduler_INTERACTIVE_BUDGET_MASK)
                 << FSScheduler_INTERACTIVE_BUDGET_SHIFT));
}

static void FSScheduler_reset_budgets(FSScheduler *scheduler) {
  scheduler->budgetsPacked = 0;
  FSScheduler_set_bg_budget(scheduler, FSScheduler_BG_BUDGET);
  FSScheduler_set_ui_budget(scheduler, FSScheduler_UI_BUDGET);
  FSScheduler_set_interactive_budget(scheduler, FSScheduler_INTERACTIVE_BUDGET);
}

static bool FSScheduler_is_task_ready(const FSSchedulerTaskRecord *task, uint64_t now_ms) {
  return task != NULL && FSSchedulerTaskRecord_callback(task) != NULL &&
         FSSchedulerTaskRecord_start_time(task) <= now_ms;
}

static int FSScheduler_total_task_count(const FSScheduler *scheduler) {
  /* total_pending semantics include READY/WAITING/PAUSED tracked tasks. */
  return scheduler->bgCount + scheduler->uiCount + scheduler->interactiveCount +
         scheduler->pausedCount;
}

static bool FSScheduler_task_is_timed_out(const FSSchedulerTaskRecord *task,
                                          uint64_t now_ms) {
  uint64_t timeout;
  if (task == NULL) {
    return false;
  }
  timeout = FSSchedulerTaskRecord_timeout(task);
  return timeout != 0 && now_ms > timeout;
}

static int FSScheduler_timed_out_slot_index(uint64_t task_id) {
  if (task_id == 0) {
    return -1;
  }
  return (int)(task_id & FSSchedulerTask_ID_SLOT_MASK);
}

/* Bitset helpers for the timedOutBits array. */
#define FS_BITSET_WORD(slot)  ((unsigned)(slot) / 64u)
#define FS_BITSET_BIT(slot)   (UINT64_C(1) << ((unsigned)(slot) % 64u))

/* Per-slot location tag (fits in 3 bits; stored as uint8_t). */
enum FSSchedulerSlotLoc {
  FSSchedulerSlotLoc_NONE              = 0,
  FSSchedulerSlotLoc_READY_BG          = 1,
  FSSchedulerSlotLoc_READY_UI          = 2,
  FSSchedulerSlotLoc_READY_INTERACTIVE = 3,
  FSSchedulerSlotLoc_WAITING           = 4,
  FSSchedulerSlotLoc_PAUSED            = 5
};

static void FSScheduler_set_slot_location(FSScheduler *scheduler,
                                          uint64_t task_id,
                                          uint8_t loc) {
  int slot = FSScheduler_timed_out_slot_index(task_id);
  if (scheduler == NULL || slot < 0 || slot >= FSScheduler_TASK_CAPACITY) {
    return;
  }
  scheduler->slotLocation[slot] = loc;
  if (loc == (uint8_t)FSSchedulerSlotLoc_NONE) {
    scheduler->slotIndex[slot] = -1;
  }
}

static uint8_t FSScheduler_get_slot_location(const FSScheduler *scheduler,
                                              uint64_t task_id) {
  int slot = FSScheduler_timed_out_slot_index(task_id);
  if (scheduler == NULL || slot < 0 || slot >= FSScheduler_TASK_CAPACITY) {
    return (uint8_t)FSSchedulerSlotLoc_NONE;
  }
  return scheduler->slotLocation[slot];
}

static void FSScheduler_set_slot_index(FSScheduler *scheduler,
                                       uint64_t task_id,
                                       int index) {
  int slot = FSScheduler_timed_out_slot_index(task_id);
  if (scheduler == NULL || slot < 0 || slot >= FSScheduler_TASK_CAPACITY) {
    return;
  }
  scheduler->slotIndex[slot] = (int16_t)index;
}

static int FSScheduler_get_slot_index(const FSScheduler *scheduler,
                                      uint64_t task_id) {
  int slot = FSScheduler_timed_out_slot_index(task_id);
  if (scheduler == NULL || slot < 0 || slot >= FSScheduler_TASK_CAPACITY) {
    return -1;
  }
  return (int)scheduler->slotIndex[slot];
}

static void FSScheduler_track_task_timeout(FSScheduler *scheduler,
                                           uint64_t timeout) {
  if (timeout == 0) {
    return;
  }
  if (scheduler->earliestTimeout == 0 || timeout < scheduler->earliestTimeout) {
    scheduler->earliestTimeout = timeout;
  }
}

static void FSScheduler_recompute_earliest_timeout(FSScheduler *scheduler) {
  FSSchedulerTaskQueue *queues[3];
  int i;
  uint64_t min_t = 0;

  queues[0] = &scheduler->bgReadyQueue;
  queues[1] = &scheduler->uiReadyQueue;
  queues[2] = &scheduler->interactiveReadyQueue;

  for (i = 0; i < 3; i++) {
    int j;
    int idx = queues[i]->head;
    for (j = 0; j < queues[i]->count; j++) {
      uint64_t t = queues[i]->tasks[idx].timeout;
      if (t != 0 && (min_t == 0 || t < min_t)) {
        min_t = t;
      }
      idx++;
      if (idx >= queues[i]->capacity) {
        idx = 0;
      }
    }
  }

  for (i = 0; i < scheduler->waitingCount; i++) {
    uint64_t t = scheduler->waitingTasks[i].timeout;
    if (t != 0 && (min_t == 0 || t < min_t)) {
      min_t = t;
    }
  }

  for (i = 0; i < scheduler->pausedCount; i++) {
    uint64_t t = scheduler->pausedTasks[i].timeout;
    if (t != 0 && (min_t == 0 || t < min_t)) {
      min_t = t;
    }
  }

  scheduler->earliestTimeout = min_t;
}

static void FSScheduler_mark_timed_out(FSScheduler *scheduler, uint64_t task_id) {
  int slot = FSScheduler_timed_out_slot_index(task_id);
  if (scheduler == NULL || slot < 0 || slot >= FSScheduler_TASK_CAPACITY) {
    return;
  }
  scheduler->timedOutBits[FS_BITSET_WORD(slot)] |= FS_BITSET_BIT(slot);
}

static void FSScheduler_clear_timed_out(FSScheduler *scheduler, uint64_t task_id) {
  int slot = FSScheduler_timed_out_slot_index(task_id);
  if (scheduler == NULL || slot < 0 || slot >= FSScheduler_TASK_CAPACITY) {
    return;
  }
  scheduler->timedOutBits[FS_BITSET_WORD(slot)] &= ~FS_BITSET_BIT(slot);
}

static bool FSScheduler_was_timed_out(const FSScheduler *scheduler,
                                      uint64_t task_id) {
  int slot = FSScheduler_timed_out_slot_index(task_id);
  if (scheduler == NULL || slot < 0 || slot >= FSScheduler_TASK_CAPACITY) {
    return false;
  }
  return (scheduler->timedOutBits[FS_BITSET_WORD(slot)] & FS_BITSET_BIT(slot)) != 0;
}

static bool FSScheduler_paused_push(FSScheduler *scheduler,
                                    const FSSchedulerTaskRecord *task) {
  if (scheduler == NULL || task == NULL) {
    return false;
  }

  if (scheduler->pausedCount >= scheduler->pausedCapacity &&
      !FSScheduler_grow_task_buffer(&scheduler->pausedTasks,
                                    &scheduler->pausedCapacity,
                                    FSScheduler_allocator(scheduler))) {
    return false;
  }

  scheduler->pausedTasks[scheduler->pausedCount++] = *task;
  FSScheduler_set_slot_location(scheduler, task->id, (uint8_t)FSSchedulerSlotLoc_PAUSED);
  FSScheduler_set_slot_index(scheduler, task->id, scheduler->pausedCount - 1);
  FSScheduler_track_task_timeout(scheduler, task->timeout);
  return true;
}

static int FSScheduler_paused_find_id(const FSScheduler *scheduler,
                                      uint64_t task_id) {
  int idx;
  if (scheduler == NULL || task_id == 0) {
    return -1;
  }
  idx = FSScheduler_get_slot_index(scheduler, task_id);
  if (idx >= 0 && idx < scheduler->pausedCount &&
      FSSchedulerTaskRecord_get_id(&scheduler->pausedTasks[idx]) == task_id) {
    return idx;
  }
  return -1;
}

static bool FSScheduler_paused_remove_at(FSScheduler *scheduler,
                                         int index,
                                         FSSchedulerTaskRecord *out_task) {
  uint64_t removed_id;
  if (scheduler == NULL || index < 0 || index >= scheduler->pausedCount) {
    return false;
  }
  removed_id = scheduler->pausedTasks[index].id;
  if (out_task != NULL) {
    *out_task = scheduler->pausedTasks[index];
  }
  scheduler->pausedCount--;
  /* Swap the last element into the vacated slot.
   * The paused list has no ordering requirement so swap-last is O(1). */
  if (index < scheduler->pausedCount) {
    scheduler->pausedTasks[index] = scheduler->pausedTasks[scheduler->pausedCount];
    FSScheduler_set_slot_index(
        scheduler, scheduler->pausedTasks[index].id, index);
  }
  FSScheduler_set_slot_location(scheduler, removed_id, (uint8_t)FSSchedulerSlotLoc_NONE);
  return true;
}

static bool FSScheduler_ready_find_earliest_deadline_index(
    const FSSchedulerTaskQueue *queue, int *out_logical_index) {
  int i;
  int idx;
  int best = -1;
  int seen_deadlines = 0;
  uint64_t best_deadline = 0;
  bool found = false;

  if (queue == NULL || out_logical_index == NULL || queue->count <= 0) {
    return false;
  }

  idx = queue->head;
  for (i = 0; i < queue->count; i++) {
    const FSSchedulerTaskRecord *task = &queue->tasks[idx];
    uint64_t deadline = FSSchedulerTaskRecord_deadline(task);
    if (deadline != 0) {
      seen_deadlines++;
      if (!found || deadline < best_deadline) {
        found = true;
        best_deadline = deadline;
        best = i;
      }
      if (seen_deadlines >= queue->deadline_count) {
        break;
      }
    }
    idx++;
    if (idx >= queue->capacity) {
      idx = 0;
    }
  }

  if (!found) {
    return false;
  }
  *out_logical_index = best;
  return true;
}

static FSSchedulerTaskQueue *FSScheduler_ready_queue_for_priority(FSScheduler *scheduler,
                                                         uint8_t budget) {
  switch (budget) {
  case FSScheduler_Priority_BACKGROUND:
    return &scheduler->bgReadyQueue;
  case FSScheduler_Priority_UI:
    return &scheduler->uiReadyQueue;
  case FSScheduler_Priority_INTERACTIVE:
    return &scheduler->interactiveReadyQueue;
  default:
    return NULL;
  }
}

static void FSScheduler_increment_priority_count(FSScheduler *scheduler,
                                                 uint8_t budget) {
  switch (budget) {
  case FSScheduler_Priority_BACKGROUND:
    scheduler->bgCount++;
    break;
  case FSScheduler_Priority_UI:
    scheduler->uiCount++;
    break;
  case FSScheduler_Priority_INTERACTIVE:
    scheduler->interactiveCount++;
    break;
  default:
    break;
  }
}

static void FSScheduler_decrement_priority_count(FSScheduler *scheduler,
                                                 uint8_t budget) {
  switch (budget) {
  case FSScheduler_Priority_BACKGROUND:
    if (scheduler->bgCount > 0) {
      scheduler->bgCount--;
    }
    break;
  case FSScheduler_Priority_UI:
    if (scheduler->uiCount > 0) {
      scheduler->uiCount--;
    }
    break;
  case FSScheduler_Priority_INTERACTIVE:
    if (scheduler->interactiveCount > 0) {
      scheduler->interactiveCount--;
    }
    break;
  default:
    break;
  }
}

static bool FSScheduler_grow_task_buffer(FSSchedulerTaskRecord **tasks, int *capacity,
                                         const FSAllocator *allocator) {
  int newCapacity;
  FSSchedulerTaskRecord *newTasks;

  if (tasks == NULL || capacity == NULL) {
    return false;
  }

  newCapacity =
      (*capacity > 0) ? (*capacity) * 2 : FSScheduler_TASK_INITIAL_CAPACITY;
  if (newCapacity > FSScheduler_TASK_CAPACITY) {
    newCapacity = FSScheduler_TASK_CAPACITY;
  }

  if (newCapacity <= *capacity) {
    return false;
  }

  newTasks = (FSSchedulerTaskRecord *)FSAllocator_reallocate(
      allocator, *tasks, (size_t)newCapacity * sizeof(FSSchedulerTaskRecord));
  if (newTasks == NULL) {
    return false;
  }

  *tasks = newTasks;
  *capacity = newCapacity;
  return true;
}

static bool FSSchedulerTaskQueue_ensure_capacity(FSSchedulerTaskQueue *queue,
                                        const FSAllocator *allocator) {
  int i;
  int srcIndex;
  FSSchedulerTaskRecord *newTasks;
  int newCapacity;

  if (queue == NULL) {
    return false;
  }

  if (queue->count < queue->capacity) {
    return true;
  }

  newCapacity = (queue->capacity > 0) ? queue->capacity * 2
                                      : FSScheduler_TASK_INITIAL_CAPACITY;
  if (newCapacity > FSScheduler_TASK_CAPACITY) {
    newCapacity = FSScheduler_TASK_CAPACITY;
  }

  if (newCapacity <= queue->capacity) {
    return false;
  }

  /* Fast path: buffer is already linear -- grow with a single realloc. */
  if (queue->head == 0) {
    newTasks = (FSSchedulerTaskRecord *)FSAllocator_reallocate(
        allocator, queue->tasks,
        (size_t)newCapacity * sizeof(FSSchedulerTaskRecord));
    if (newTasks == NULL) {
      return false;
    }
    queue->tasks = newTasks;
    queue->capacity = newCapacity;
    /* head stays 0; after growing a full queue, the next insertion point is
     * the logical end (count), not the wrapped tail value from the old
     * capacity. */
    queue->tail = queue->count;
    return true;
  }

  /* Slow path: circular buffer must be linearised before growing. */
  newTasks =
      (FSSchedulerTaskRecord *)FSAllocator_allocate(allocator,
                                     (size_t)newCapacity * sizeof(FSSchedulerTaskRecord));
  if (newTasks == NULL) {
    return false;
  }

  srcIndex = queue->head;
  for (i = 0; i < queue->count; i++) {
    newTasks[i] = queue->tasks[srcIndex];
    srcIndex++;
    if (srcIndex >= queue->capacity) {
      srcIndex = 0;
    }
  }

  FSAllocator_deallocate(allocator, queue->tasks);
  queue->tasks = newTasks;
  queue->capacity = newCapacity;
  queue->head = 0;
  queue->tail = queue->count;
  return true;
}

static void FSSchedulerTaskQueue_shrink_if_sparse(FSSchedulerTaskQueue *queue,
                                         const FSAllocator *allocator) {
  int newCapacity;
  int i;
  int srcIndex;
  FSSchedulerTaskRecord *newTasks;

  if (queue == NULL || queue->capacity <= FSScheduler_TASK_INITIAL_CAPACITY) {
    return;
  }

  if (queue->count > (queue->capacity / 8)) {
    return;
  }

  newCapacity = queue->capacity / 2;
  if (newCapacity < FSScheduler_TASK_INITIAL_CAPACITY) {
    newCapacity = FSScheduler_TASK_INITIAL_CAPACITY;
  }

  if (newCapacity >= queue->capacity || queue->count > newCapacity) {
    return;
  }

  /* Fast path: buffer is already linear -- shrink with a single realloc. */
  if (queue->head == 0) {
    newTasks = (FSSchedulerTaskRecord *)FSAllocator_reallocate(
        allocator, queue->tasks,
        (size_t)newCapacity * sizeof(FSSchedulerTaskRecord));
    if (newTasks == NULL) {
      return;
    }
    queue->tasks = newTasks;
    queue->capacity = newCapacity;
    queue->tail = queue->count;
    return;
  }

  /* Slow path: circular buffer must be linearised before shrinking. */
  newTasks = (FSSchedulerTaskRecord *)FSAllocator_allocate(
      allocator, (size_t)newCapacity * sizeof(FSSchedulerTaskRecord));
  if (newTasks == NULL) {
    return;
  }

  srcIndex = queue->head;
  for (i = 0; i < queue->count; i++) {
    newTasks[i] = queue->tasks[srcIndex];
    srcIndex++;
    if (srcIndex >= queue->capacity) {
      srcIndex = 0;
    }
  }

  FSAllocator_deallocate(allocator, queue->tasks);
  queue->tasks = newTasks;
  queue->capacity = newCapacity;
  queue->head = 0;
  queue->tail = queue->count;
}

static void FSSchedulerTaskQueue_clear(FSSchedulerTaskQueue *queue,
                              const FSAllocator *allocator) {
  if (queue == NULL) {
    return;
  }

  FSAllocator_deallocate(allocator, queue->tasks);
  queue->tasks = NULL;
  queue->count = 0;
  queue->capacity = 0;
  queue->head = 0;
  queue->tail = 0;
  queue->deadline_count = 0;
}

static bool FSSchedulerTaskQueue_push(FSSchedulerTaskQueue *queue, const FSSchedulerTaskRecord *task,
                             const FSAllocator *allocator) {
  int nextTail;

  if (queue == NULL || task == NULL) {
    return false;
  }

  if (!FSSchedulerTaskQueue_ensure_capacity(queue, allocator)) {
    return false;
  }

  queue->tasks[queue->tail] = *task;
  if (task->deadline != 0) {
    queue->deadline_count++;
  }
  nextTail = queue->tail + 1;
  if (nextTail >= queue->capacity) {
    nextTail = 0;
  }
  queue->tail = nextTail;
  queue->count++;
  return true;
}

static bool FSSchedulerTaskQueue_pop(FSSchedulerTaskQueue *queue, FSSchedulerTaskRecord *task) {
  int nextHead;

  if (queue == NULL || queue->count <= 0 || task == NULL) {
    return false;
  }

  *task = queue->tasks[queue->head];
  if (task->deadline != 0) {
    queue->deadline_count--;
  }
  nextHead = queue->head + 1;
  if (nextHead >= queue->capacity) {
    nextHead = 0;
  }
  queue->head = nextHead;
  queue->count--;

  if (queue->count == 0) {
    queue->head = 0;
    queue->tail = 0;
  }

  return true;
}

/* Returns the logical (0-based) position of task_id in the queue, or -1. */
static int FSSchedulerTaskQueue_find_id(const FSScheduler *scheduler,
                                        const FSSchedulerTaskQueue *queue,
                                        uint64_t task_id) {
  int idx;

  if (scheduler == NULL || queue == NULL || task_id == 0 || queue->count == 0) {
    return -1;
  }

  idx = FSScheduler_get_slot_index(scheduler, task_id);
  if (idx < 0 || idx >= queue->capacity) {
    return -1;
  }
  if (FSSchedulerTaskRecord_get_id(&queue->tasks[idx]) != task_id) {
    return -1;
  }
  if (idx >= queue->head) {
    return idx - queue->head;
  }
  return idx + (queue->capacity - queue->head);
}

static bool FSSchedulerTaskQueue_remove_at(FSScheduler *scheduler,
                                           FSSchedulerTaskQueue *queue,
                                           int logical_pos) {
  int i;
  int phys;

  if (queue == NULL || logical_pos < 0 || logical_pos >= queue->count) {
    return false;
  }

  phys = queue->head + logical_pos;
  if (phys >= queue->capacity) {
    phys -= queue->capacity;
  }
  if (queue->tasks[phys].deadline != 0) {
    queue->deadline_count--;
  }

  if (logical_pos == 0) {
    queue->head++;
    if (queue->head >= queue->capacity) {
      queue->head = 0;
    }
    queue->count--;
    if (queue->count == 0) {
      queue->head = 0;
      queue->tail = 0;
    }
    return true;
  }

  /* Shift [removed+1 .. tail-1] one slot toward head, then retreat tail. */
  for (i = logical_pos; i < queue->count - 1; i++) {
    int dst = queue->head + i;
    int src = dst + 1;
    if (dst >= queue->capacity) {
      dst -= queue->capacity;
    }
    if (src >= queue->capacity) {
      src -= queue->capacity;
    }
    queue->tasks[dst] = queue->tasks[src];
    FSScheduler_set_slot_index(scheduler, queue->tasks[dst].id, dst);
  }

  queue->tail--;
  if (queue->tail < 0) {
    queue->tail = queue->capacity - 1;
  }

  queue->count--;
  if (queue->count == 0) {
    queue->head = 0;
    queue->tail = 0;
  }
  return true;
}


static bool FSScheduler_waiting_less(const FSSchedulerTaskRecord *a, const FSSchedulerTaskRecord *b) {
  uint64_t a_start;
  uint64_t b_start;
  uint64_t a_deadline;
  uint64_t b_deadline;
  uint8_t a_priority;
  uint8_t b_priority;

  a_start = FSSchedulerTaskRecord_start_time(a);
  b_start = FSSchedulerTaskRecord_start_time(b);
  if (a_start != b_start) {
    return a_start < b_start;
  }

  a_priority = FSSchedulerTaskRecord_priority(a);
  b_priority = FSSchedulerTaskRecord_priority(b);
  if (a_priority == b_priority) {
    a_deadline = FSSchedulerTaskRecord_deadline(a);
    b_deadline = FSSchedulerTaskRecord_deadline(b);
    if (a_deadline != 0 && b_deadline != 0 && a_deadline != b_deadline) {
      return a_deadline < b_deadline;
    }
    if (a_deadline != 0 && b_deadline == 0) {
      return true;
    }
    if (a_deadline == 0 && b_deadline != 0) {
      return false;
    }
  }
  return a_priority > b_priority;
}

static void FSScheduler_waiting_sift_up(FSScheduler *scheduler,
                                        FSSchedulerTaskRecord tasks[],
                                        int index) {
  while (index > 0) {
    int parent = (index - 1) / 2;
    FSSchedulerTaskRecord tmp;

    if (!FSScheduler_waiting_less(&tasks[index], &tasks[parent])) {
      break;
    }

    tmp = tasks[parent];
    tasks[parent] = tasks[index];
    tasks[index] = tmp;
    FSScheduler_set_slot_index(scheduler, tasks[parent].id, parent);
    FSScheduler_set_slot_index(scheduler, tasks[index].id, index);
    index = parent;
  }
}

static void FSScheduler_waiting_sift_down(FSScheduler *scheduler,
                                          FSSchedulerTaskRecord tasks[],
                                          int count,
                                          int index) {
  for (;;) {
    int left = (index * 2) + 1;
    int right = left + 1;
    int smallest = index;
    FSSchedulerTaskRecord tmp;

    if (left < count &&
        FSScheduler_waiting_less(&tasks[left], &tasks[smallest])) {
      smallest = left;
    }

    if (right < count &&
        FSScheduler_waiting_less(&tasks[right], &tasks[smallest])) {
      smallest = right;
    }

    if (smallest == index) {
      break;
    }

    tmp = tasks[index];
    tasks[index] = tasks[smallest];
    tasks[smallest] = tmp;
    FSScheduler_set_slot_index(scheduler, tasks[index].id, index);
    FSScheduler_set_slot_index(scheduler, tasks[smallest].id, smallest);
    index = smallest;
  }
}

static void FSScheduler_refresh_earliest_wake(FSScheduler *scheduler) {
  if (scheduler->waitingCount > 0) {
    scheduler->has_earliest_wake_time = true;
    scheduler->earliest_wake_time =
        FSSchedulerTaskRecord_start_time(&scheduler->waitingTasks[0]);
  } else {
    scheduler->has_earliest_wake_time = false;
    scheduler->earliest_wake_time = 0;
  }
}

static bool FSScheduler_waiting_push(FSScheduler *scheduler,
                                     const FSSchedulerTaskRecord *task) {
  if (scheduler->waitingCount >= scheduler->waitingCapacity &&
      !FSScheduler_grow_task_buffer(&scheduler->waitingTasks,
                                    &scheduler->waitingCapacity,
                                    FSScheduler_allocator(scheduler))) {
    return false;
  }

  scheduler->waitingTasks[scheduler->waitingCount] = *task;
  FSScheduler_set_slot_index(scheduler, task->id, scheduler->waitingCount);
  FSScheduler_waiting_sift_up(scheduler, scheduler->waitingTasks, scheduler->waitingCount);
  scheduler->waitingCount++;

  FSScheduler_set_slot_location(scheduler, task->id, (uint8_t)FSSchedulerSlotLoc_WAITING);
  FSScheduler_track_task_timeout(scheduler, task->timeout);
  FSScheduler_refresh_earliest_wake(scheduler);
  return true;
}

static void FSScheduler_waiting_shrink_if_sparse(FSScheduler *scheduler) {
  int newCapacity;
  FSSchedulerTaskRecord *newTasks;

  if (scheduler == NULL ||
      scheduler->waitingCapacity <= FSScheduler_TASK_INITIAL_CAPACITY) {
    return;
  }

  if (scheduler->waitingCount > (scheduler->waitingCapacity / 8)) {
    return;
  }

  newCapacity = scheduler->waitingCapacity / 2;
  if (newCapacity < FSScheduler_TASK_INITIAL_CAPACITY) {
    newCapacity = FSScheduler_TASK_INITIAL_CAPACITY;
  }

  if (newCapacity >= scheduler->waitingCapacity ||
      scheduler->waitingCount > newCapacity) {
    return;
  }

  newTasks = (FSSchedulerTaskRecord *)FSAllocator_reallocate(
      FSScheduler_allocator(scheduler), scheduler->waitingTasks,
      (size_t)newCapacity * sizeof(FSSchedulerTaskRecord));
  if (newTasks == NULL) {
    return;
  }

  scheduler->waitingTasks = newTasks;
  scheduler->waitingCapacity = newCapacity;
}

static bool FSScheduler_waiting_pop(FSScheduler *scheduler, FSSchedulerTaskRecord *task) {
  if (scheduler == NULL || task == NULL || scheduler->waitingCount <= 0) {
    return false;
  }

  *task = scheduler->waitingTasks[0];
  scheduler->waitingCount--;

  if (scheduler->waitingCount > 0) {
    scheduler->waitingTasks[0] =
        scheduler->waitingTasks[scheduler->waitingCount];
    FSScheduler_set_slot_index(scheduler, scheduler->waitingTasks[0].id, 0);
    FSScheduler_waiting_sift_down(scheduler, scheduler->waitingTasks,
                                  scheduler->waitingCount, 0);
  }

  FSScheduler_set_slot_location(scheduler, task->id, (uint8_t)FSSchedulerSlotLoc_NONE);
  FSScheduler_refresh_earliest_wake(scheduler);
  FSScheduler_waiting_shrink_if_sparse(scheduler);
  return true;
}

/* Returns the heap index of task_id in the waiting heap, or -1. */
static int FSScheduler_waiting_find_id(const FSScheduler *scheduler,
                                       uint64_t task_id) {
  int idx;

  if (scheduler == NULL || task_id == 0) {
    return -1;
  }

  idx = FSScheduler_get_slot_index(scheduler, task_id);
  if (idx >= 0 && idx < scheduler->waitingCount &&
      FSSchedulerTaskRecord_get_id(&scheduler->waitingTasks[idx]) == task_id) {
    return idx;
  }

  return -1;
}

/* Removes the element at heap index from the waiting min-heap and re-heapifies. */
static void FSScheduler_waiting_remove_at(FSScheduler *scheduler, int index) {
  uint64_t removed_id;
  if (scheduler == NULL || index < 0 || index >= scheduler->waitingCount) {
    return;
  }

  removed_id = scheduler->waitingTasks[index].id;
  scheduler->waitingCount--;

  if (index < scheduler->waitingCount) {
    scheduler->waitingTasks[index] =
        scheduler->waitingTasks[scheduler->waitingCount];
    FSScheduler_set_slot_index(scheduler, scheduler->waitingTasks[index].id, index);
    if (index > 0 &&
        FSScheduler_waiting_less(&scheduler->waitingTasks[index],
                                 &scheduler->waitingTasks[(index - 1) / 2])) {
      FSScheduler_waiting_sift_up(scheduler, scheduler->waitingTasks, index);
    } else {
      FSScheduler_waiting_sift_down(scheduler, scheduler->waitingTasks,
                                    scheduler->waitingCount, index);
    }
  }

  FSScheduler_set_slot_location(scheduler, removed_id, (uint8_t)FSSchedulerSlotLoc_NONE);
  FSScheduler_refresh_earliest_wake(scheduler);
  FSScheduler_waiting_shrink_if_sparse(scheduler);
}

static bool FSScheduler_enqueue_task_without_count(FSScheduler *scheduler,
                                                   const FSSchedulerTaskRecord *task,
                                                   uint64_t now_ms) {
  FSSchedulerTaskQueue *readyQueue;
  uint8_t priority;

  if (scheduler == NULL || task == NULL) {
    return false;
  }

  priority = FSSchedulerTaskRecord_priority(task);
  readyQueue = FSScheduler_ready_queue_for_priority(scheduler, priority);
  if (readyQueue == NULL) {
    return false;
  }

  if (FSScheduler_is_task_ready(task, now_ms)) {
    int inserted_idx = readyQueue->tail;
    if (!FSSchedulerTaskQueue_push(readyQueue, task, FSScheduler_allocator(scheduler))) {
      return false;
    }
    {
      static const uint8_t s_priority_loc[3] = {
          (uint8_t)FSSchedulerSlotLoc_READY_BG,
          (uint8_t)FSSchedulerSlotLoc_READY_UI,
          (uint8_t)FSSchedulerSlotLoc_READY_INTERACTIVE
      };
      if (priority < 3) {
        FSScheduler_set_slot_location(scheduler, task->id, s_priority_loc[priority]);
        FSScheduler_set_slot_index(scheduler, task->id, inserted_idx);
      }
    }
    FSScheduler_track_task_timeout(scheduler, task->timeout);
    return true;
  }

  return FSScheduler_waiting_push(scheduler, task);
}

static uint64_t FSScheduler_add_task_internal(FSScheduler *scheduler,
                                              FSSchedulerTaskRecord task,
                                              bool count_task) {
  uint64_t now_ms;
  uint8_t priority;

  if (scheduler == NULL || FSSchedulerTaskRecord_callback(&task) == NULL) {
    return 0;
  }

  now_ms = FSScheduler_current_time_ms(scheduler);

  if (task.kind != FSSchedulerTaskKind_INSTANT &&
      FSSchedulerTaskRecord_start_time(&task) == 0) {
    FSSchedulerTaskRecord_set_start_time(&task, now_ms);
  }

  if (!FSScheduler_assign_task_id(scheduler, &task)) {
    return 0;
  }
  FSScheduler_set_slot_location(scheduler, FSSchedulerTaskRecord_get_id(&task),
                                (uint8_t)FSSchedulerSlotLoc_NONE);
  FSScheduler_clear_timed_out(scheduler, FSSchedulerTaskRecord_get_id(&task));

  if (FSScheduler_total_task_count(scheduler) >= FSScheduler_TASK_CAPACITY) {
    return 0;
  }

  if (!FSScheduler_enqueue_task_without_count(scheduler, &task, now_ms)) {
    return 0;
  }

  if (count_task) {
    priority = FSSchedulerTaskRecord_priority(&task);
    FSScheduler_increment_priority_count(scheduler, priority);
  }

  return task.id;
}

static bool FSScheduler_promote_ready_waiting_tasks(FSScheduler *scheduler,
                                                    uint64_t now_ms) {
  while (scheduler->waitingCount > 0 &&
         FSSchedulerTaskRecord_start_time(&scheduler->waitingTasks[0]) <=
             now_ms) {
    FSSchedulerTaskRecord task;
    FSSchedulerTaskQueue *queue;
    uint8_t priority;

    if (!FSScheduler_waiting_pop(scheduler, &task)) {
      return false;
    }

    priority = FSSchedulerTaskRecord_priority(&task);
    queue = FSScheduler_ready_queue_for_priority(scheduler, priority);
    if (queue == NULL ||
        !FSSchedulerTaskQueue_push(queue, &task, FSScheduler_allocator(scheduler))) {
      (void)FSScheduler_waiting_push(scheduler, &task);
      return false;
    }
    {
      int inserted_idx = queue->tail - 1;
      static const uint8_t s_ploc[3] = {
          (uint8_t)FSSchedulerSlotLoc_READY_BG,
          (uint8_t)FSSchedulerSlotLoc_READY_UI,
          (uint8_t)FSSchedulerSlotLoc_READY_INTERACTIVE
      };
      uint8_t p = FSSchedulerTaskRecord_priority(&task);
      if (inserted_idx < 0) {
        inserted_idx = queue->capacity - 1;
      }
      if (p < 3) {
        FSScheduler_set_slot_location(scheduler, task.id, s_ploc[p]);
        FSScheduler_set_slot_index(scheduler, task.id, inserted_idx);
      }
    }
  }

  return true;
}

static bool FSScheduler_has_ready_tasks(const FSScheduler *scheduler) {
  return scheduler->interactiveReadyQueue.count > 0 ||
         scheduler->uiReadyQueue.count > 0 || scheduler->bgReadyQueue.count > 0;
}

static void FSScheduler_purge_timed_out_tasks(FSScheduler *scheduler,
                                              uint64_t now_ms) {
  FSSchedulerTaskQueue *queues[3];
  int i;

  if (scheduler == NULL) {
    return;
  }

  if (scheduler->earliestTimeout == 0 || now_ms < scheduler->earliestTimeout) {
    return;
  }

  queues[0] = &scheduler->bgReadyQueue;
  queues[1] = &scheduler->uiReadyQueue;
  queues[2] = &scheduler->interactiveReadyQueue;
  for (i = 0; i < 3; i++) {
    int pos = 0;
    while (pos < queues[i]->count) {
      int idx = queues[i]->head + pos;
      FSSchedulerTaskRecord task;
      if (idx >= queues[i]->capacity) {
        idx -= queues[i]->capacity;
      }
      task = queues[i]->tasks[idx];
      if (FSScheduler_task_is_timed_out(&task, now_ms)) {
        uint8_t priority = FSSchedulerTaskRecord_priority(&task);
        uint64_t id = FSSchedulerTaskRecord_get_id(&task);
        (void)FSSchedulerTaskQueue_remove_at(scheduler, queues[i], pos);
        FSScheduler_set_slot_location(scheduler, id, (uint8_t)FSSchedulerSlotLoc_NONE);
        FSScheduler_decrement_priority_count(scheduler, priority);
        FSScheduler_mark_timed_out(scheduler, id);
        continue;
      }
      pos++;
    }
    FSSchedulerTaskQueue_shrink_if_sparse(queues[i], FSScheduler_allocator(scheduler));
  }

  {
    int idx = 0;
    while (idx < scheduler->waitingCount) {
      FSSchedulerTaskRecord task = scheduler->waitingTasks[idx];
      if (FSScheduler_task_is_timed_out(&task, now_ms)) {
        uint8_t priority = FSSchedulerTaskRecord_priority(&task);
        uint64_t id = FSSchedulerTaskRecord_get_id(&task);
        FSScheduler_waiting_remove_at(scheduler, idx);
        FSScheduler_decrement_priority_count(scheduler, priority);
        FSScheduler_mark_timed_out(scheduler, id);
        continue;
      }
      idx++;
    }
  }

  {
    int idx = 0;
    while (idx < scheduler->pausedCount) {
      FSSchedulerTaskRecord task = scheduler->pausedTasks[idx];
      if (FSScheduler_task_is_timed_out(&task, now_ms)) {
        uint8_t priority = FSSchedulerTaskRecord_priority(&task);
        uint64_t id = FSSchedulerTaskRecord_get_id(&task);
        (void)FSScheduler_paused_remove_at(scheduler, idx, NULL);
        FSScheduler_decrement_priority_count(scheduler, priority);
        FSScheduler_mark_timed_out(scheduler, id);
        continue;
      }
      idx++;
    }
  }

  FSScheduler_recompute_earliest_timeout(scheduler);
}

static bool
FSScheduler_all_ready_tasks_blocked_by_budget(const FSScheduler *scheduler) {
  int interactive_budget = FSScheduler_get_interactive_budget(scheduler);
  int ui_budget = FSScheduler_get_ui_budget(scheduler);
  int bg_budget = FSScheduler_get_bg_budget(scheduler);
  bool interactive_blocked =
      scheduler->interactiveReadyQueue.count <= 0 || interactive_budget <= 0;
  bool ui_blocked = scheduler->uiReadyQueue.count <= 0 || ui_budget <= 0;
  bool bg_blocked = scheduler->bgReadyQueue.count <= 0 || bg_budget <= 0;

  return interactive_blocked && ui_blocked && bg_blocked;
}

static bool FSScheduler_dispatch_from_ready_queue(FSScheduler *scheduler,
                                                   FSSchedulerTaskQueue *queue,
                                                   uint8_t budget_level, int budget,
                                                   uint64_t now_ms) {
  FSSchedulerTaskRecord taskToRun;
  void (*task_fn)(void *context);
  void *task_context;
  uint8_t priority;
  uint64_t reschedule_base_ms;
  uint64_t start_time;
  uint64_t execute_cycle;
  int logical_idx = -1;

  if (scheduler == NULL || queue == NULL || budget <= 0 || queue->count <= 0) {
    return false;
  }

  if (queue->deadline_count > 0 && FSScheduler_ready_find_earliest_deadline_index(queue, &logical_idx)) {
    int physical_idx = queue->head + logical_idx;
    if (physical_idx >= queue->capacity) {
      physical_idx -= queue->capacity;
    }
    taskToRun = queue->tasks[physical_idx];
    if (!FSSchedulerTaskQueue_remove_at(scheduler, queue, logical_idx)) {
      return false;
    }
  } else if (!FSSchedulerTaskQueue_pop(queue, &taskToRun)) {
    return false;
  }
  FSScheduler_set_slot_location(scheduler, FSSchedulerTaskRecord_get_id(&taskToRun), (uint8_t)FSSchedulerSlotLoc_NONE);
  FSSchedulerTaskQueue_shrink_if_sparse(queue, FSScheduler_allocator(scheduler));

  switch (budget_level) {
  case FSScheduler_Priority_INTERACTIVE:
    FSScheduler_set_interactive_budget(scheduler, budget - 1);
    break;
  case FSScheduler_Priority_UI:
    FSScheduler_set_ui_budget(scheduler, budget - 1);
    break;
  case FSScheduler_Priority_BACKGROUND:
    FSScheduler_set_bg_budget(scheduler, budget - 1);
    break;
  default:
    return false;
  }

  task_fn = FSSchedulerTaskRecord_callback(&taskToRun);
  task_context = FSSchedulerTaskRecord_context(&taskToRun);
  priority = FSSchedulerTaskRecord_priority(&taskToRun);
  if (task_fn == NULL) {
    return false;
  }

  if (FSScheduler_task_is_timed_out(&taskToRun, now_ms)) {
    FSScheduler_decrement_priority_count(scheduler, priority);
    FSScheduler_mark_timed_out(scheduler, FSSchedulerTaskRecord_get_id(&taskToRun));
    return true;
  }

  FSScheduler_clear_timed_out(scheduler, FSSchedulerTaskRecord_get_id(&taskToRun));
  task_fn(task_context);

  if (!FSSchedulerTaskRecord_is_repeating(&taskToRun)) {
    FSScheduler_decrement_priority_count(scheduler, priority);
    return true;
  }

  start_time = FSSchedulerTaskRecord_start_time(&taskToRun);
  execute_cycle = FSSchedulerTaskRecord_execute_cycle(&taskToRun);
  if (FSSchedulerTaskRecord_repeat_mode(&taskToRun) ==
      FSSchedulerTaskRepeat_FIXEDRATE) {
    /* Arithmetic catch-up: compute next scheduled start without looping.
     * now_ms is the time captured before dispatch; the next start must be
     * strictly after it to avoid immediately re-running a stale task. */
    uint64_t next_start = start_time + execute_cycle;
    if (next_start <= now_ms) {
      uint64_t elapsed = now_ms - start_time;
      uint64_t skipped = elapsed / execute_cycle;
      next_start = start_time + (skipped + 1) * execute_cycle;
    }
    FSSchedulerTaskRecord_set_start_time(&taskToRun, next_start);
    reschedule_base_ms = now_ms;
  } else {
    /* FIXED_DELAY: measure delay from the moment the task finished. */
    reschedule_base_ms = FSScheduler_current_time_ms(scheduler);
    FSSchedulerTaskRecord_set_start_time(&taskToRun,
                                         reschedule_base_ms + execute_cycle);
  }

  if (!FSScheduler_enqueue_task_without_count(scheduler, &taskToRun,
                                              reschedule_base_ms)) {
    FSScheduler_decrement_priority_count(scheduler, priority);
    return false;
  }

  return true;
}

void FSScheduler_init(struct FSScheduler *scheduler) {
  FSScheduler_init_with_allocator(scheduler, NULL);
}

void FSScheduler_init_with_allocator(FSScheduler *scheduler,
                                     const FSAllocator *allocator) {
  const FSAllocator *alloc;
  FSSchedulerTaskRecord *buf;

  if (scheduler == NULL) {
    return;
  }

  scheduler->allocator = *FSAllocator_resolve(allocator);
  alloc = FSScheduler_allocator(scheduler);

  /* Pre-allocate every queue and the waiting heap to the initial capacity so
   * the first burst of task additions does not trigger reallocation. */
  buf = (FSSchedulerTaskRecord *)FSAllocator_allocate(
      alloc, FSScheduler_TASK_INITIAL_CAPACITY * sizeof(FSSchedulerTaskRecord));
  scheduler->bgReadyQueue =
      buf ? (FSSchedulerTaskQueue){buf, 0, FSScheduler_TASK_INITIAL_CAPACITY, 0, 0, 0}
          : (FSSchedulerTaskQueue){NULL, 0, 0, 0, 0, 0};

  buf = (FSSchedulerTaskRecord *)FSAllocator_allocate(
      alloc, FSScheduler_TASK_INITIAL_CAPACITY * sizeof(FSSchedulerTaskRecord));
  scheduler->uiReadyQueue =
      buf ? (FSSchedulerTaskQueue){buf, 0, FSScheduler_TASK_INITIAL_CAPACITY, 0, 0, 0}
          : (FSSchedulerTaskQueue){NULL, 0, 0, 0, 0, 0};

  buf = (FSSchedulerTaskRecord *)FSAllocator_allocate(
      alloc, FSScheduler_TASK_INITIAL_CAPACITY * sizeof(FSSchedulerTaskRecord));
  scheduler->interactiveReadyQueue =
      buf ? (FSSchedulerTaskQueue){buf, 0, FSScheduler_TASK_INITIAL_CAPACITY, 0, 0, 0}
          : (FSSchedulerTaskQueue){NULL, 0, 0, 0, 0, 0};

  buf = (FSSchedulerTaskRecord *)FSAllocator_allocate(
      alloc, FSScheduler_TASK_INITIAL_CAPACITY * sizeof(FSSchedulerTaskRecord));
  scheduler->waitingTasks = buf;
  scheduler->waitingCount = 0;
  scheduler->waitingCapacity =
      buf ? FSScheduler_TASK_INITIAL_CAPACITY : 0;
  scheduler->pausedTasks = NULL;
  scheduler->pausedCount = 0;
  scheduler->pausedCapacity = 0;

  scheduler->bgCount = 0;
  scheduler->uiCount = 0;
  scheduler->interactiveCount = 0;

  scheduler->has_earliest_wake_time = false;
  scheduler->earliest_wake_time = 0;
  scheduler->earliestTimeout = 0;

  scheduler->nextSlot = 0;
  scheduler->generation = 0;
  memset(scheduler->timedOutBits, 0, sizeof(scheduler->timedOutBits));
  memset(scheduler->slotLocation, 0, sizeof(scheduler->slotLocation));
  {
    int i;
    for (i = 0; i < FSScheduler_TASK_CAPACITY; i++) {
      scheduler->slotIndex[i] = -1;
    }
  }
  scheduler->time_provider = &FSTime_init;
  scheduler->now_fn = FSScheduler_default_now_fn;
  scheduler->now_context = scheduler;
  FSScheduler_reset_budgets(scheduler);
}

void FSScheduler_deinit(struct FSScheduler *scheduler) {
  const FSAllocator *allocator;

  if (scheduler == NULL) {
    return;
  }

  allocator = FSScheduler_allocator(scheduler);

  FSSchedulerTaskQueue_clear(&scheduler->bgReadyQueue, allocator);
  FSSchedulerTaskQueue_clear(&scheduler->uiReadyQueue, allocator);
  FSSchedulerTaskQueue_clear(&scheduler->interactiveReadyQueue, allocator);

  FSAllocator_deallocate(allocator, scheduler->waitingTasks);
  scheduler->waitingTasks = NULL;
  scheduler->waitingCount = 0;
  scheduler->waitingCapacity = 0;
  FSAllocator_deallocate(allocator, scheduler->pausedTasks);
  scheduler->pausedTasks = NULL;
  scheduler->pausedCount = 0;
  scheduler->pausedCapacity = 0;

  scheduler->bgCount = 0;
  scheduler->uiCount = 0;
  scheduler->interactiveCount = 0;

  scheduler->has_earliest_wake_time = false;
  scheduler->earliest_wake_time = 0;
  scheduler->earliestTimeout = 0;

  scheduler->budgetsPacked = 0;

  scheduler->allocator = (FSAllocator){0};
  scheduler->nextSlot = 0;
  scheduler->generation = 0;
  memset(scheduler->timedOutBits, 0, sizeof(scheduler->timedOutBits));
  memset(scheduler->slotLocation, 0, sizeof(scheduler->slotLocation));
  {
    int i;
    for (i = 0; i < FSScheduler_TASK_CAPACITY; i++) {
      scheduler->slotIndex[i] = -1;
    }
  }
  scheduler->time_provider = NULL;
  /* now_fn == NULL is used by callers to detect an uninitialised/deinitialized
   * scheduler; it must be cleared last. */
  scheduler->now_fn = NULL;
  scheduler->now_context = NULL;
}
uint64_t FSScheduler_add_instant_task(FSScheduler *scheduler,
                                      FSSchedulerInstantTask task) {
  FSSchedulerTaskRecord t = {0};

  if (scheduler == NULL || task.task == NULL) {
    return 0;
  }

  if (task.priority != FSScheduler_Priority_BACKGROUND &&
      task.priority != FSScheduler_Priority_UI &&
      task.priority != FSScheduler_Priority_INTERACTIVE) {
    return 0;
  }

  t.kind        = (uint8_t)FSSchedulerTaskKind_INSTANT;
  t.task        = task.task;
  t.context     = task.context;
  t.id          = 0;
  t.start_time  = 0;
  t.execute_cycle = 0;
  t.deadline    = task.deadline;
  t.timeout     = task.timeout;
  t.priority    = task.priority;
  t.repeat_mode = (uint8_t)FSSchedulerTaskRepeat_FIXEDDELAY;

  return FSScheduler_add_task_internal(scheduler, t, true);
}

FSSchedulerTaskHandler
FSScheduler_add_instant_task_handle(FSScheduler *scheduler,
                                    FSSchedulerInstantTask task) {
  FSSchedulerTaskHandler handle = FSSchedulerTaskHandler_init;
  handle.task_id = FSScheduler_add_instant_task(scheduler, task);
  return handle;
}

uint64_t FSScheduler_add_deferred_task(FSScheduler *scheduler,
                                       FSSchedulerDeferredTask task) {
  FSSchedulerTaskRecord t = {0};

  if (scheduler == NULL || task.task == NULL) {
    return 0;
  }

  if (task.priority != FSScheduler_Priority_BACKGROUND &&
      task.priority != FSScheduler_Priority_UI &&
      task.priority != FSScheduler_Priority_INTERACTIVE) {
    return 0;
  }

  t.kind        = (uint8_t)FSSchedulerTaskKind_DEFERRED;
  t.task        = task.task;
  t.context     = task.context;
  t.id          = 0;
  t.start_time  = task.start_time;
  t.execute_cycle = 0;
  t.deadline    = task.deadline;
  t.timeout     = task.timeout;
  t.priority    = task.priority;
  t.repeat_mode = (uint8_t)FSSchedulerTaskRepeat_FIXEDDELAY;

  return FSScheduler_add_task_internal(scheduler, t, true);
}

FSSchedulerTaskHandler
FSScheduler_add_deferred_task_handle(FSScheduler *scheduler,
                                     FSSchedulerDeferredTask task) {
  FSSchedulerTaskHandler handle = FSSchedulerTaskHandler_init;
  handle.task_id = FSScheduler_add_deferred_task(scheduler, task);
  return handle;
}

uint64_t FSScheduler_add_repeating_task(FSScheduler *scheduler,
                                        FSSchedulerRepeatingTask task) {
  FSSchedulerTaskRecord t = {0};

  if (scheduler == NULL || task.task == NULL) {
    return 0;
  }

  if (task.execute_cycle == 0) {
    return 0;
  }

  if (task.repeat_mode != FSSchedulerTaskRepeat_FIXEDDELAY &&
      task.repeat_mode != FSSchedulerTaskRepeat_FIXEDRATE) {
    return 0;
  }

  if (task.priority != FSScheduler_Priority_BACKGROUND &&
      task.priority != FSScheduler_Priority_UI &&
      task.priority != FSScheduler_Priority_INTERACTIVE) {
    return 0;
  }

  t.kind        = (uint8_t)FSSchedulerTaskKind_REPEATING;
  t.task        = task.task;
  t.context     = task.context;
  t.id          = 0;
  t.start_time  = task.start_time;
  t.execute_cycle = task.execute_cycle;
  t.deadline    = task.deadline;
  t.timeout     = task.timeout;
  t.priority    = task.priority;
  t.repeat_mode = (uint8_t)task.repeat_mode;

  return FSScheduler_add_task_internal(scheduler, t, true);
}

FSSchedulerTaskHandler
FSScheduler_add_repeating_task_handle(FSScheduler *scheduler,
                                      FSSchedulerRepeatingTask task) {
  FSSchedulerTaskHandler handle = FSSchedulerTaskHandler_init;
  handle.task_id = FSScheduler_add_repeating_task(scheduler, task);
  return handle;
}

bool FSScheduler_set_time_source(FSScheduler *scheduler,
                                 uint64_t (*now_fn)(void *context),
                                 void *context) {
  if (scheduler == NULL || now_fn == NULL) {
    return false;
  }

  scheduler->now_fn = now_fn;
  scheduler->now_context = context;
  return true;
}

bool FSScheduler_set_time_provider(FSScheduler *scheduler,
                                   const FSTime *provider) {
  if (scheduler == NULL || provider == NULL ||
      provider->now_monotonic_ms == NULL || provider->sleep_ms == NULL) {
    return false;
  }

  scheduler->time_provider = provider;
  /* Reset to the default now_fn so the scheduler uses the new provider. */
  scheduler->now_fn     = FSScheduler_default_now_fn;
  scheduler->now_context = scheduler;
  return true;
}

bool FSScheduler_has_pending_tasks(const FSScheduler *scheduler) {
  if (scheduler == NULL) {
    return false;
  }

  return scheduler->interactiveCount > 0 || scheduler->uiCount > 0 ||
         scheduler->bgCount > 0 || scheduler->pausedCount > 0;
}

bool FSScheduler_next_sleep_ms(const FSScheduler *scheduler,
                               uint64_t *out_delay_ms) {
  uint64_t now_ms;

  if (scheduler == NULL || out_delay_ms == NULL) {
    return false;
  }

  if (scheduler->interactiveReadyQueue.count > 0 ||
      scheduler->uiReadyQueue.count > 0 || scheduler->bgReadyQueue.count > 0) {
    *out_delay_ms = 0;
    return true;
  }

  if (!scheduler->has_earliest_wake_time) {
    *out_delay_ms = 0;
    return false;
  }

  now_ms = FSScheduler_current_time_ms(scheduler);
  if (scheduler->earliest_wake_time <= now_ms) {
    *out_delay_ms = 0;
  } else {
    *out_delay_ms = scheduler->earliest_wake_time - now_ms;
  }

  return true;
}

bool FSScheduler_process_for_ms(FSScheduler *scheduler, uint64_t duration_ms) {
  uint64_t start_ms;
  uint64_t deadline_ms;
  uint64_t now_ms;

  if (scheduler == NULL) {
    return false;
  }

  start_ms = FSScheduler_current_time_ms(scheduler);
  deadline_ms = start_ms + duration_ms;
  now_ms = start_ms;

  /* now_ms is kept up-to-date after each blocking operation so the while
   * condition reuses it instead of calling current_time_ms a second time
   * per iteration. */
  while (now_ms < deadline_ms) {
    uint64_t sleep_ms;

    if (FSScheduler_step(scheduler)) {
      now_ms = FSScheduler_current_time_ms(scheduler);
      continue;
    }

    if (!FSScheduler_next_sleep_ms(scheduler, &sleep_ms)) {
      break;
    }

    if (sleep_ms == 0) {
      now_ms = FSScheduler_current_time_ms(scheduler);
      continue;
    }

    if (sleep_ms > deadline_ms - now_ms) {
      sleep_ms = deadline_ms - now_ms;
    }

    if (scheduler->time_provider != NULL &&
        scheduler->time_provider->sleep_ms != NULL) {
      (void)scheduler->time_provider->sleep_ms(sleep_ms);
    }
    now_ms = FSScheduler_current_time_ms(scheduler);
  }

  return true;
}

bool FSScheduler_step(struct FSScheduler *scheduler) {
  uint64_t now_ms;
  int interactive_budget;
  int ui_budget;
  int bg_budget;

  if (scheduler == NULL) {
    return false;
  }

  /* Single time query reused for promotion and all dispatch catch-up
   * calculations within this step. */
  now_ms = FSScheduler_current_time_ms(scheduler);
  FSScheduler_purge_timed_out_tasks(scheduler, now_ms);

  if (scheduler->waitingCount > 0) {
    if (!FSScheduler_promote_ready_waiting_tasks(scheduler, now_ms)) {
      return false;
    }
  }

  if (!FSScheduler_has_ready_tasks(scheduler)) {
    return false;
  }

  if (FSScheduler_all_ready_tasks_blocked_by_budget(scheduler)) {
    FSScheduler_reset_budgets(scheduler);
  }

  interactive_budget = FSScheduler_get_interactive_budget(scheduler);
  if (scheduler->interactiveReadyQueue.count > 0 && interactive_budget > 0) {
    return FSScheduler_dispatch_from_ready_queue(
        scheduler, &scheduler->interactiveReadyQueue,
        FSScheduler_Priority_INTERACTIVE, interactive_budget, now_ms);
  }

  ui_budget = FSScheduler_get_ui_budget(scheduler);
  if (scheduler->uiReadyQueue.count > 0 && ui_budget > 0) {
    return FSScheduler_dispatch_from_ready_queue(
        scheduler, &scheduler->uiReadyQueue, FSScheduler_Priority_UI,
        ui_budget, now_ms);
  }

  bg_budget = FSScheduler_get_bg_budget(scheduler);
  if (scheduler->bgReadyQueue.count > 0 && bg_budget > 0) {
    return FSScheduler_dispatch_from_ready_queue(
        scheduler, &scheduler->bgReadyQueue, FSScheduler_Priority_BACKGROUND,
        bg_budget, now_ms);
  }

  return false;
}

/* Locate task_id in the three priority-ready queues. On success, copies the
 * record to *out_task (if non-NULL), removes it from its queue, calls
 * FSSchedulerTaskQueue_shrink_if_sparse on that queue, and returns the queue pointer.
 * Returns NULL if the task is not found in any ready queue. */
static FSSchedulerTaskQueue *FSScheduler_find_and_remove_from_ready_queues(
    FSScheduler *scheduler, uint64_t task_id, FSSchedulerTaskRecord *out_task) {
  FSSchedulerTaskQueue *queue = NULL;
  int logical_pos;
  int physical_idx;

  switch (FSScheduler_get_slot_location(scheduler, task_id)) {
  case FSSchedulerSlotLoc_READY_BG:
    queue = &scheduler->bgReadyQueue;
    break;
  case FSSchedulerSlotLoc_READY_UI:
    queue = &scheduler->uiReadyQueue;
    break;
  case FSSchedulerSlotLoc_READY_INTERACTIVE:
    queue = &scheduler->interactiveReadyQueue;
    break;
  default:
    return NULL;
  }

  logical_pos = FSSchedulerTaskQueue_find_id(scheduler, queue, task_id);
  if (logical_pos < 0) {
    return NULL;
  }
  physical_idx = queue->head + logical_pos;
  if (physical_idx >= queue->capacity) {
    physical_idx -= queue->capacity;
  }
  if (out_task != NULL) {
    *out_task = queue->tasks[physical_idx];
  }
  if (!FSSchedulerTaskQueue_remove_at(scheduler, queue, logical_pos)) {
    return NULL;
  }
  FSScheduler_set_slot_location(scheduler, task_id, (uint8_t)FSSchedulerSlotLoc_NONE);
  FSSchedulerTaskQueue_shrink_if_sparse(queue, FSScheduler_allocator(scheduler));
  return queue;
}

FSSchedulerComponentMemorySnapshot
FSScheduler_component_memory_snapshot(const FSScheduler *scheduler) {
  FSSchedulerComponentMemorySnapshot snapshot = {0, 0, 0, 0, 0};
  size_t bg_bytes;
  size_t ui_bytes;
  size_t interactive_bytes;
  size_t waiting_bytes;

  if (scheduler == NULL) {
    return snapshot;
  }

  bg_bytes = (scheduler->bgReadyQueue.capacity > 0)
                 ? ((size_t)scheduler->bgReadyQueue.capacity * sizeof(FSSchedulerTaskRecord))
                 : 0;
  ui_bytes = (scheduler->uiReadyQueue.capacity > 0)
                 ? ((size_t)scheduler->uiReadyQueue.capacity * sizeof(FSSchedulerTaskRecord))
                 : 0;
  interactive_bytes = (scheduler->interactiveReadyQueue.capacity > 0)
                          ? ((size_t)scheduler->interactiveReadyQueue.capacity *
                             sizeof(FSSchedulerTaskRecord))
                          : 0;
  waiting_bytes = (scheduler->waitingCapacity > 0)
                      ? ((size_t)scheduler->waitingCapacity * sizeof(FSSchedulerTaskRecord))
                      : 0;

  snapshot.background_queue_bytes = bg_bytes;
  snapshot.ui_queue_bytes = ui_bytes;
  snapshot.interactive_queue_bytes = interactive_bytes;
  snapshot.waiting_heap_bytes = waiting_bytes;
  snapshot.total_bytes =
      bg_bytes + ui_bytes + interactive_bytes + waiting_bytes;
  return snapshot;
}

bool FSScheduler_cancel_task(FSScheduler *scheduler, uint64_t task_id) {
  FSSchedulerTaskRecord found_task;
  uint8_t priority;

  if (scheduler == NULL || task_id == 0) {
    return false;
  }

  if (FSScheduler_find_and_remove_from_ready_queues(scheduler, task_id,
                                                    &found_task) != NULL) {
    priority = FSSchedulerTaskRecord_priority(&found_task);
    FSScheduler_decrement_priority_count(scheduler, priority);
    FSScheduler_clear_timed_out(scheduler, task_id);
    return true;
  }

  {
    int waiting_idx = FSScheduler_waiting_find_id(scheduler, task_id);
    if (waiting_idx >= 0) {
      priority =
          FSSchedulerTaskRecord_priority(&scheduler->waitingTasks[waiting_idx]);
      FSScheduler_waiting_remove_at(scheduler, waiting_idx);
      FSScheduler_decrement_priority_count(scheduler, priority);
      FSScheduler_clear_timed_out(scheduler, task_id);
      return true;
    }
  }

  {
    int paused_idx = FSScheduler_paused_find_id(scheduler, task_id);
    if (paused_idx >= 0) {
      if (FSScheduler_paused_remove_at(scheduler, paused_idx, NULL)) {
        FSScheduler_clear_timed_out(scheduler, task_id);
        return true;
      }
    }
  }

  return false;
}

bool FSScheduler_pause_task(FSScheduler *scheduler, uint64_t task_id) {
  FSSchedulerTaskRecord found_task;
  uint8_t priority;

  if (scheduler == NULL || task_id == 0) {
    return false;
  }

  if (FSScheduler_find_and_remove_from_ready_queues(scheduler, task_id,
                                                    &found_task) != NULL) {
    if (!FSScheduler_paused_push(scheduler, &found_task)) {
      uint64_t now_ms = FSScheduler_current_time_ms(scheduler);
      (void)FSScheduler_enqueue_task_without_count(scheduler, &found_task, now_ms);
      return false;
    }
    priority = FSSchedulerTaskRecord_priority(&found_task);
    FSScheduler_decrement_priority_count(scheduler, priority);
    return true;
  }

  {
    int waiting_idx = FSScheduler_waiting_find_id(scheduler, task_id);
    if (waiting_idx >= 0) {
      found_task = scheduler->waitingTasks[waiting_idx];
      /* Remove from waiting first (sets location NONE), then push to paused
       * (sets location PAUSED). If paused_push fails, re-push to waiting. */
      FSScheduler_waiting_remove_at(scheduler, waiting_idx);
      if (!FSScheduler_paused_push(scheduler, &found_task)) {
        (void)FSScheduler_waiting_push(scheduler, &found_task);
        return false;
      }
      priority = FSSchedulerTaskRecord_priority(&found_task);
      FSScheduler_decrement_priority_count(scheduler, priority);
      return true;
    }
  }

  return false;
}

bool FSScheduler_resume_task(FSScheduler *scheduler, uint64_t task_id) {
  int paused_idx;
  FSSchedulerTaskRecord task;
  uint64_t now_ms;
  uint8_t priority;

  if (scheduler == NULL || task_id == 0) {
    return false;
  }

  paused_idx = FSScheduler_paused_find_id(scheduler, task_id);
  if (paused_idx < 0) {
    return false;
  }

  if (!FSScheduler_paused_remove_at(scheduler, paused_idx, &task)) {
    return false;
  }
  now_ms = FSScheduler_current_time_ms(scheduler);
  if (FSScheduler_task_is_timed_out(&task, now_ms)) {
    FSScheduler_mark_timed_out(scheduler, task_id);
    return true;
  }
  if (!FSScheduler_enqueue_task_without_count(scheduler, &task, now_ms)) {
    (void)FSScheduler_paused_push(scheduler, &task);
    return false;
  }
  priority = FSSchedulerTaskRecord_priority(&task);
  FSScheduler_increment_priority_count(scheduler, priority);
  return true;
}

static bool FSScheduler_update_task_record_start_time(FSScheduler *scheduler,
                                                      uint64_t task_id,
                                                      uint64_t start_time_ms) {
  int waiting_idx;
  int paused_idx;
  FSSchedulerTaskRecord task;
  uint64_t now_ms = FSScheduler_current_time_ms(scheduler);

  if (scheduler == NULL || task_id == 0) {
    return false;
  }

  if (FSScheduler_find_and_remove_from_ready_queues(scheduler, task_id,
                                                    &task) != NULL) {
    FSSchedulerTaskRecord_set_start_time(&task, start_time_ms);
    return FSScheduler_enqueue_task_without_count(scheduler, &task, now_ms);
  }

  waiting_idx = FSScheduler_waiting_find_id(scheduler, task_id);
  if (waiting_idx >= 0) {
    task = scheduler->waitingTasks[waiting_idx];
    FSScheduler_waiting_remove_at(scheduler, waiting_idx);
    FSSchedulerTaskRecord_set_start_time(&task, start_time_ms);
    return FSScheduler_enqueue_task_without_count(scheduler, &task, now_ms);
  }

  paused_idx = FSScheduler_paused_find_id(scheduler, task_id);
  if (paused_idx >= 0) {
    FSSchedulerTaskRecord_set_start_time(&scheduler->pausedTasks[paused_idx],
                                         start_time_ms);
    return true;
  }

  return false;
}

bool FSScheduler_reschedule_task(FSScheduler *scheduler, uint64_t task_id,
                                 uint64_t start_time_ms) {
  return FSScheduler_update_task_record_start_time(scheduler, task_id,
                                                   start_time_ms);
}

static bool FSScheduler_set_task_time_field(FSScheduler *scheduler,
                                            uint64_t task_id,
                                            uint64_t value,
                                            bool is_deadline) {
  int waiting_idx;
  int paused_idx;
  FSSchedulerTaskRecord task;
  uint64_t now_ms = FSScheduler_current_time_ms(scheduler);

  if (scheduler == NULL || task_id == 0) {
    return false;
  }

  if (FSScheduler_find_and_remove_from_ready_queues(scheduler, task_id,
                                                    &task) != NULL) {
    if (is_deadline) {
      FSSchedulerTaskRecord_set_deadline(&task, value);
    } else {
      FSSchedulerTaskRecord_set_timeout(&task, value);
    }
    return FSScheduler_enqueue_task_without_count(scheduler, &task, now_ms);
  }

  waiting_idx = FSScheduler_waiting_find_id(scheduler, task_id);
  if (waiting_idx >= 0) {
    task = scheduler->waitingTasks[waiting_idx];
    FSScheduler_waiting_remove_at(scheduler, waiting_idx);
    if (is_deadline) {
      FSSchedulerTaskRecord_set_deadline(&task, value);
    } else {
      FSSchedulerTaskRecord_set_timeout(&task, value);
    }
    return FSScheduler_enqueue_task_without_count(scheduler, &task, now_ms);
  }

  paused_idx = FSScheduler_paused_find_id(scheduler, task_id);
  if (paused_idx >= 0) {
    if (is_deadline) {
      FSSchedulerTaskRecord_set_deadline(&scheduler->pausedTasks[paused_idx],
                                         value);
    } else {
      FSSchedulerTaskRecord_set_timeout(&scheduler->pausedTasks[paused_idx],
                                        value);
      FSScheduler_track_task_timeout(scheduler, value);
    }
    return true;
  }

  return false;
}

bool FSScheduler_set_task_deadline(FSScheduler *scheduler, uint64_t task_id,
                                   uint64_t deadline_ms) {
  return FSScheduler_set_task_time_field(scheduler, task_id, deadline_ms, true);
}

bool FSScheduler_set_task_timeout(FSScheduler *scheduler, uint64_t task_id,
                                  uint64_t timeout_ms) {
  return FSScheduler_set_task_time_field(scheduler, task_id, timeout_ms, false);
}

FSSchedulerTaskStatus FSScheduler_task_status(const FSScheduler *scheduler,
                                              uint64_t task_id) {
  uint8_t loc;
  if (scheduler == NULL || task_id == 0) {
    return FSSchedulerTaskStatus_NOT_FOUND;
  }
  loc = FSScheduler_get_slot_location(scheduler, task_id);
  switch (loc) {
  case FSSchedulerSlotLoc_READY_BG:
  case FSSchedulerSlotLoc_READY_UI:
  case FSSchedulerSlotLoc_READY_INTERACTIVE:
    return FSSchedulerTaskStatus_PENDING_READY;
  case FSSchedulerSlotLoc_WAITING:
    return FSSchedulerTaskStatus_PENDING_WAITING;
  case FSSchedulerSlotLoc_PAUSED:
    return FSSchedulerTaskStatus_PENDING_PAUSED;
  default:
    if (FSScheduler_was_timed_out(scheduler, task_id)) {
      return FSSchedulerTaskStatus_TIMED_OUT;
    }
    return FSSchedulerTaskStatus_NOT_FOUND;
  }
}

bool FSScheduler_task_handle_is_valid(const FSSchedulerTaskHandler *handle) {
  return handle != NULL && handle->task_id != 0;
}

uint64_t FSScheduler_task_handle_user_tag(const FSSchedulerTaskHandler *handle) {
  if (handle == NULL) {
    return 0;
  }
  return handle->user_tag;
}

bool FSScheduler_task_handle_set_user_tag(FSSchedulerTaskHandler *handle,
                                          uint64_t user_tag) {
  if (handle == NULL) {
    return false;
  }
  handle->user_tag = user_tag;
  return true;
}

bool FSScheduler_task_handle_cancel(FSScheduler *scheduler,
                                    FSSchedulerTaskHandler *handle) {
  if (!FSScheduler_task_handle_is_valid(handle)) {
    return false;
  }
  return FSScheduler_cancel_task(scheduler, handle->task_id);
}

bool FSScheduler_task_handle_pause(FSScheduler *scheduler,
                                   const FSSchedulerTaskHandler *handle) {
  if (!FSScheduler_task_handle_is_valid(handle)) {
    return false;
  }
  return FSScheduler_pause_task(scheduler, handle->task_id);
}

bool FSScheduler_task_handle_resume(FSScheduler *scheduler,
                                    const FSSchedulerTaskHandler *handle) {
  if (!FSScheduler_task_handle_is_valid(handle)) {
    return false;
  }
  return FSScheduler_resume_task(scheduler, handle->task_id);
}

bool FSScheduler_task_handle_reschedule(
    FSScheduler *scheduler, const FSSchedulerTaskHandler *handle,
    uint64_t start_time_ms) {
  if (!FSScheduler_task_handle_is_valid(handle)) {
    return false;
  }
  return FSScheduler_reschedule_task(scheduler, handle->task_id, start_time_ms);
}

bool FSScheduler_task_handle_set_deadline(
    FSScheduler *scheduler, const FSSchedulerTaskHandler *handle,
    uint64_t deadline_ms) {
  if (!FSScheduler_task_handle_is_valid(handle)) {
    return false;
  }
  return FSScheduler_set_task_deadline(scheduler, handle->task_id, deadline_ms);
}

bool FSScheduler_task_handle_set_timeout(
    FSScheduler *scheduler, const FSSchedulerTaskHandler *handle,
    uint64_t timeout_ms) {
  if (!FSScheduler_task_handle_is_valid(handle)) {
    return false;
  }
  return FSScheduler_set_task_timeout(scheduler, handle->task_id, timeout_ms);
}

FSSchedulerTaskStatus FSScheduler_task_handle_status(
    const FSScheduler *scheduler, const FSSchedulerTaskHandler *handle) {
  if (!FSScheduler_task_handle_is_valid(handle)) {
    return FSSchedulerTaskStatus_NOT_FOUND;
  }
  return FSScheduler_task_status(scheduler, handle->task_id);
}

FSSchedulerStateSnapshot FSScheduler_state_snapshot(
    const FSScheduler *scheduler) {
  FSSchedulerStateSnapshot snapshot;

  memset(&snapshot, 0, sizeof(snapshot));

  if (scheduler == NULL) {
    return snapshot;
  }

  snapshot.background_ready_count  = scheduler->bgReadyQueue.count;
  snapshot.ui_ready_count          = scheduler->uiReadyQueue.count;
  snapshot.interactive_ready_count = scheduler->interactiveReadyQueue.count;
  snapshot.waiting_count           = scheduler->waitingCount;
  snapshot.paused_count            = scheduler->pausedCount;
  snapshot.total_pending =
      scheduler->bgCount + scheduler->uiCount + scheduler->interactiveCount +
      scheduler->pausedCount;
  snapshot.has_earliest_wake_time = scheduler->has_earliest_wake_time;
  snapshot.earliest_wake_time_ms  = scheduler->earliest_wake_time;

  return snapshot;
}
