#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "Feather.h"
#include "FeatherRuntime/FSResourceTracker.h"

static uint64_t g_fake_now_ms = 0;
static int g_counter = 0;

static uint64_t fake_now(void *context) {
  (void)context;
  return g_fake_now_ms;
}

static void count_task(void *context) {
  int *counter = (int *)context;
  (*counter)++;
}

static bool expect_true(bool condition, const char *message) {
  if (!condition) {
    printf("[FAIL] %s\n", message);
    return false;
  }

  printf("[PASS] %s\n", message);
  return true;
}

static bool run_waiting_heap_tracking(void) {
  Feather feather;
  FeatherConfig config = FeatherConfig_init;
  FSResourceTracker tracking;
  FSResourceTrackerSnapshot snapshot;
  /* 4 pre-allocated buffers: 3 ready queues + 1 waiting heap */
  FSResourceTrackerRecord records[4];
  size_t record_count;
  size_t r;
  bool ok = true;
  int i;

  g_counter = 0;
  g_fake_now_ms = 1000;

  ok = expect_true(FSResourceTracker_init(&tracking),
                   "tracking: initialize tracker") &&
       ok;

  config.allocator = FSResourceTracker_allocator(&tracking);
  config.now_fn = fake_now;
  ok = expect_true(Feather_init_with_config(&feather, &config),
                   "tracking: initialize Feather with tracked allocator") &&
       ok;

  for (i = 0; i < 48; i++) {
    ok = expect_true(
             Feather_add_deferred_task(
                 &feather,
                 (FSSchedulerDeferredTask){
                     .task = count_task,
                     .context = &g_counter,
                     .priority = FSScheduler_Priority_BACKGROUND,
                     .start_time = g_fake_now_ms + 500 + (uint64_t)i}),
             "tracking: add deferred task") &&
         ok;
  }

  snapshot = FSResourceTracker_snapshot(&tracking);
  /* Init pre-allocates all four buffers (3 ready queues + waiting heap); the
   * waiting heap may have grown via realloc to hold 48 tasks but the record
   * count stays at 4. */
  ok = expect_true(snapshot.active_allocations == 4,
                   "tracking: waiting heap uses one active allocation") &&
       ok;
  ok = expect_true(snapshot.current_bytes >= 48 * sizeof(FSSchedulerTaskRecord),
                   "tracking: waiting heap reports current bytes") &&
       ok;
  ok = expect_true(FSResourceTracker_has_leaks(&tracking),
                   "tracking: active allocations are visible before deinit") &&
       ok;
  record_count =
      FSResourceTracker_copy_active_records(&tracking, records, 4);
  ok = expect_true(record_count == 4,
                   "tracking: leak snapshot reports four active records") &&
       ok;
  /* Verify that the sum of all live record sizes equals current_bytes. */
  {
    size_t total = 0;
    for (r = 0; r < record_count; r++) {
      total += records[r].size;
    }
    ok = expect_true(total == snapshot.current_bytes,
                     "tracking: record size matches current tracked bytes") &&
         ok;
  }

  g_fake_now_ms = 200000;
  while (Feather_step(&feather)) {
  }
  snapshot = FSResourceTracker_snapshot(&tracking);
  ok = expect_true(snapshot.active_allocations == 4,
                   "tracking: waiting heap drain keeps allocator records stable") &&
       ok;
  {
    FeatherComponentMemorySnapshot component_snapshot =
        Feather_component_memory_snapshot(&feather);
    ok = expect_true(
             component_snapshot.waiting_heap_bytes ==
                 FSScheduler_TASK_INITIAL_CAPACITY * sizeof(FSSchedulerTaskRecord),
             "tracking: waiting heap shrinks back to initial capacity after drain") &&
         ok;
  }

  Feather_deinit(&feather);

  snapshot = FSResourceTracker_snapshot(&tracking);
  ok = expect_true(snapshot.active_allocations == 0,
                   "tracking: deinit releases waiting heap allocation") &&
       ok;
  ok = expect_true(snapshot.current_bytes == 0,
                   "tracking: deinit clears current tracked bytes") &&
       ok;
  ok = expect_true(snapshot.peak_bytes > 0,
                   "tracking: peak bytes captured during scheduler lifetime") &&
       ok;
  ok = expect_true(snapshot.total_allocated_bytes ==
                       snapshot.total_freed_bytes,
                   "tracking: allocated and freed bytes balance after deinit") &&
       ok;

  FSResourceTracker_deinit(&tracking);
  return ok;
}

static bool run_ready_queue_tracking(void) {
  Feather feather;
  FeatherConfig config = FeatherConfig_init;
  FSResourceTracker tracking;
  FSResourceTrackerSnapshot snapshot;
  bool ok = true;

  g_counter = 0;

  ok = expect_true(FSResourceTracker_init(&tracking),
                   "tracking-ready: initialize tracker") &&
       ok;

  config.allocator = FSResourceTracker_allocator(&tracking);
  ok = expect_true(Feather_init_with_config(&feather, &config),
                   "tracking-ready: initialize Feather with tracker") &&
       ok;

  ok = expect_true(
           Feather_add_instant_task(
               &feather,
               (FSSchedulerInstantTask){.task = count_task,
                                        .context = &g_counter,
                                        .priority = FSScheduler_Priority_BACKGROUND}),
           "tracking-ready: add background task") &&
       ok;
  ok = expect_true(
           Feather_add_instant_task(
               &feather,
               (FSSchedulerInstantTask){.task = count_task,
                                        .context = &g_counter,
                                        .priority = FSScheduler_Priority_UI}),
           "tracking-ready: add UI task") &&
       ok;
  ok = expect_true(
           Feather_add_instant_task(
               &feather,
               (FSSchedulerInstantTask){.task = count_task,
                                        .context = &g_counter,
                                        .priority = FSScheduler_Priority_INTERACTIVE}),
           "tracking-ready: add interactive task") &&
       ok;

  snapshot = FSResourceTracker_snapshot(&tracking);
  /* Init pre-allocates all 4 buffers (3 ready queues + waiting heap); adding
   * immediate tasks into the pre-allocated queues creates no new allocations. */
  ok = expect_true(snapshot.active_allocations == 4,
                   "tracking-ready: all pre-allocated buffers are tracked (3 ready queues + waiting heap)") &&
       ok;
  {
    FeatherComponentMemorySnapshot component_snapshot =
        Feather_component_memory_snapshot(&feather);
    ok = expect_true(component_snapshot.background_queue_bytes ==
                         FSScheduler_TASK_INITIAL_CAPACITY * sizeof(FSSchedulerTaskRecord),
                     "tracking-ready: background queue component bytes tracked") &&
         ok;
    ok = expect_true(component_snapshot.ui_queue_bytes ==
                         FSScheduler_TASK_INITIAL_CAPACITY * sizeof(FSSchedulerTaskRecord),
                     "tracking-ready: UI queue component bytes tracked") &&
         ok;
    ok = expect_true(component_snapshot.interactive_queue_bytes ==
                         FSScheduler_TASK_INITIAL_CAPACITY * sizeof(FSSchedulerTaskRecord),
                     "tracking-ready: interactive queue component bytes tracked") &&
         ok;
    ok = expect_true(component_snapshot.waiting_heap_bytes ==
                         FSScheduler_TASK_INITIAL_CAPACITY * sizeof(FSSchedulerTaskRecord),
                     "tracking-ready: waiting heap component bytes tracked") &&
         ok;
    ok = expect_true(
             component_snapshot.total_bytes ==
                 (component_snapshot.background_queue_bytes +
                  component_snapshot.ui_queue_bytes +
                  component_snapshot.interactive_queue_bytes +
                  component_snapshot.waiting_heap_bytes),
             "tracking-ready: total component bytes equals component sum") &&
         ok;
  }

  while (Feather_step(&feather)) {
  }

  ok = expect_true(g_counter == 3,
                   "tracking-ready: all ready queue tasks executed") &&
       ok;
  {
    FeatherComponentMemorySnapshot component_snapshot =
        Feather_component_memory_snapshot(&feather);
    ok = expect_true(
             component_snapshot.total_bytes ==
                 (component_snapshot.background_queue_bytes +
                  component_snapshot.ui_queue_bytes +
                  component_snapshot.interactive_queue_bytes +
                  component_snapshot.waiting_heap_bytes),
              "tracking-ready: component snapshot remains internally consistent") &&
          ok;
  }

  {
    int i;
    for (i = 0; i < 64; i++) {
      ok = expect_true(
               Feather_add_instant_task(
                   &feather,
                   (FSSchedulerInstantTask){.task = count_task,
                                            .context = &g_counter,
                                            .priority = FSScheduler_Priority_UI}),
               "tracking-ready: add UI burst task") &&
           ok;
    }
  }
  while (Feather_step(&feather)) {
  }
  {
    FeatherComponentMemorySnapshot component_snapshot =
        Feather_component_memory_snapshot(&feather);
    ok = expect_true(
             component_snapshot.ui_queue_bytes ==
                 FSScheduler_TASK_INITIAL_CAPACITY * sizeof(FSSchedulerTaskRecord),
             "tracking-ready: UI ready queue shrinks back to initial capacity after drain") &&
         ok;
  }

  Feather_deinit(&feather);

  snapshot = FSResourceTracker_snapshot(&tracking);
  ok = expect_true(snapshot.current_bytes == 0,
                   "tracking-ready: deinit clears ready queue allocations") &&
       ok;
  {
    FeatherComponentMemorySnapshot component_snapshot =
        Feather_component_memory_snapshot(&feather);
    ok = expect_true(component_snapshot.background_queue_bytes == 0 &&
                         component_snapshot.ui_queue_bytes == 0 &&
                         component_snapshot.interactive_queue_bytes == 0 &&
                         component_snapshot.waiting_heap_bytes == 0 &&
                         component_snapshot.total_bytes == 0,
                     "tracking-ready: deinitialized Feather component snapshot is zeroed") &&
         ok;
  }

  FSResourceTracker_deinit(&tracking);
  return ok;
}

static bool run_scheduler_snapshot(void) {
  Feather feather;
  FeatherConfig config = FeatherConfig_init;
  FSResourceTracker tracking;
  FSResourceTrackerSchedulerSnapshot combined;
  uint64_t id_ready;
  uint64_t id_waiting;
  bool ok = true;

  g_counter = 0;
  g_fake_now_ms = 0;

  ok = expect_true(FSResourceTracker_init(&tracking),
                   "scheduler-snapshot: initialize tracker") &&
       ok;

  config.allocator = FSResourceTracker_allocator(&tracking);
  config.now_fn = fake_now;
  ok = expect_true(Feather_init_with_config(&feather, &config),
                   "scheduler-snapshot: initialize Feather with tracked allocator") &&
       ok;

  /* Empty scheduler: memory allocated, no tasks pending */
  combined =
      FSResourceTracker_scheduler_snapshot(&tracking, &feather.scheduler);
  ok = expect_true(combined.scheduler.total_pending == 0,
                   "scheduler-snapshot: empty scheduler has 0 pending") &&
       ok;
  ok = expect_true(combined.memory.active_allocations == 4,
                   "scheduler-snapshot: 4 pre-allocated buffers visible") &&
       ok;

  /* Add one ready and one waiting task */
  id_ready = Feather_add_instant_task(
      &feather,
      (FSSchedulerInstantTask){.task = count_task,
                               .context = &g_counter,
                               .priority = FSScheduler_Priority_UI});
  id_waiting = Feather_add_deferred_task(
      &feather,
      (FSSchedulerDeferredTask){.task = count_task,
                                .context = &g_counter,
                                .priority = FSScheduler_Priority_BACKGROUND,
                                .start_time = 500});
  ok = expect_true(id_ready > 0 && id_waiting > 0,
                   "scheduler-snapshot: tasks enqueued") &&
       ok;

  combined =
      FSResourceTracker_scheduler_snapshot(&tracking, &feather.scheduler);
  ok = expect_true(combined.scheduler.total_pending == 2,
                   "scheduler-snapshot: 2 total pending tasks") &&
       ok;
  ok = expect_true(combined.scheduler.ui_ready_count == 1,
                   "scheduler-snapshot: 1 UI ready task") &&
       ok;
  ok = expect_true(combined.scheduler.waiting_count == 1,
                   "scheduler-snapshot: 1 waiting task") &&
       ok;
  ok = expect_true(combined.scheduler.has_earliest_wake_time,
                   "scheduler-snapshot: has earliest wake time") &&
       ok;
  ok = expect_true(combined.scheduler.earliest_wake_time_ms == 500,
                   "scheduler-snapshot: earliest wake time is 500") &&
       ok;
  ok = expect_true(combined.memory.current_bytes > 0,
                   "scheduler-snapshot: memory bytes are tracked") &&
       ok;

  /* Cancel the ready task; snapshot reflects the change */
  ok = expect_true(FSScheduler_cancel_task(&feather.scheduler, id_ready),
                   "scheduler-snapshot: cancel ready task succeeds") &&
       ok;
  combined =
      FSResourceTracker_scheduler_snapshot(&tracking, &feather.scheduler);
  ok = expect_true(combined.scheduler.ui_ready_count == 0,
                   "scheduler-snapshot: 0 UI ready tasks after cancel") &&
       ok;
  ok = expect_true(combined.scheduler.total_pending == 1,
                   "scheduler-snapshot: 1 pending after cancel") &&
       ok;

  Feather_deinit(&feather);
  FSResourceTracker_deinit(&tracking);
  return ok;
}

int main(void) {
  bool ok = true;

  ok = run_waiting_heap_tracking() && ok;
  ok = run_ready_queue_tracking() && ok;
  ok = run_scheduler_snapshot() && ok;

  if (ok) {
    puts("[PASS] Feather resource tracking tests");
    return EXIT_SUCCESS;
  }

  puts("[FAIL] Feather resource tracking tests");
  return EXIT_FAILURE;
}
