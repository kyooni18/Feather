#define _POSIX_C_SOURCE 200809L

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Feather.h"

#define DEADLINE_ORDER_TASK_COUNT 2

static uint64_t g_fake_now_ms = 0;
static int g_counter = 0;
static char g_deadline_order[DEADLINE_ORDER_TASK_COUNT];
static int g_deadline_order_index = 0;

static uint64_t fake_now(void *context) {
  (void)context;
  return g_fake_now_ms;
}

static void count_task(void *context) {
  int *counter = (int *)context;
  (*counter)++;
}

static void deadline_task_a(void *context) {
  (void)context;
  g_deadline_order[g_deadline_order_index++] = 'A';
}

static void deadline_task_b(void *context) {
  (void)context;
  g_deadline_order[g_deadline_order_index++] = 'B';
}

static bool parse_u64_arg(const char *flag, int argc, char **argv,
                          uint64_t *value) {
  int i;

  for (i = 2; i < argc - 1; i++) {
    if (strcmp(argv[i], flag) == 0) {
      char *end = NULL;
      unsigned long long parsed = strtoull(argv[i + 1], &end, 10);
      if (end == argv[i + 1] || *end != '\0') {
        return false;
      }
      *value = (uint64_t)parsed;
      return true;
    }
  }

  return true;
}

static bool parse_repeat_mode_arg(int argc, char **argv,
                                  FSSchedulerTaskRepeatMode *out_mode) {
  int i;

  for (i = 2; i < argc - 1; i++) {
    if (strcmp(argv[i], "--mode") != 0) {
      continue;
    }

    if (strcmp(argv[i + 1], "delay") == 0) {
      *out_mode = FSSchedulerTaskRepeat_FIXEDDELAY;
      return true;
    }
    if (strcmp(argv[i + 1], "rate") == 0) {
      *out_mode = FSSchedulerTaskRepeat_FIXEDRATE;
      return true;
    }
    return false;
  }

  return true;
}

static bool expect_true(bool condition, const char *message) {
  if (!condition) {
    printf("[FAIL] %s\n", message);
    return false;
  }

  printf("[PASS] %s\n", message);
  return true;
}

static bool run_once(void) {
  struct Feather feather;
  FSSchedulerInstantTask firstTask = {.task = count_task,
                                      .context = &g_counter,
                                      .priority = FSScheduler_Priority_UI};
  FSSchedulerInstantTask secondTask = {.task = count_task,
                                       .context = &g_counter,
                                       .priority = FSScheduler_Priority_UI};
  bool ok = true;
  uint64_t first_id = 0;
  uint64_t second_id = 0;

  g_counter = 0;
  Feather_init(&feather);

  first_id = Feather_add_instant_task(&feather, firstTask);
  ok = expect_true(first_id > 0,
                   "once: add first immediate task") &&
       ok;
  second_id = Feather_add_instant_task(&feather, secondTask);
  ok = expect_true(second_id > 0,
                   "once: add second immediate task") &&
       ok;

  ok = expect_true(firstTask.id == 0 && secondTask.id == 0,
                   "once: caller-owned task copies remain unchanged") &&
       ok;
  ok = expect_true(feather.scheduler.uiReadyQueue.count == 2,
                   "once: two tasks queued") &&
       ok;
  ok = expect_true(feather.scheduler.uiReadyQueue.tasks[0].id > 0,
                   "once: first queued task gets a positive ID") &&
       ok;
  ok = expect_true(feather.scheduler.uiReadyQueue.tasks[1].id >
                       feather.scheduler.uiReadyQueue.tasks[0].id,
                   "once: queued tasks receive unique increasing IDs") &&
       ok;
  ok = expect_true(first_id == feather.scheduler.uiReadyQueue.tasks[0].id &&
                       second_id == feather.scheduler.uiReadyQueue.tasks[1].id,
                   "once: add_task returns queued task IDs") &&
       ok;

  ok = expect_true(Feather_step(&feather), "once: first step executes task") &&
       ok;
  ok = expect_true(Feather_step(&feather), "once: second step executes task") &&
       ok;
  ok = expect_true(g_counter == 2, "once: counter incremented twice") && ok;
  ok = expect_true(!Feather_step(&feather), "once: queue drained") && ok;

  Feather_deinit(&feather);
  return ok;
}

static bool run_delayed(uint64_t delay_ms) {
  struct Feather feather;
  bool ok = true;

  g_counter = 0;
  g_fake_now_ms = 1000;

  Feather_init(&feather);
  ok = expect_true(Feather_set_time_source(&feather, fake_now, NULL),
                   "delayed: set fake time source") &&
       ok;
  ok = expect_true(
           Feather_add_deferred_task(
               &feather,
               (FSSchedulerDeferredTask){.task = count_task,
                                         .context = &g_counter,
                                         .priority = FSScheduler_Priority_UI,
                                         .start_time = g_fake_now_ms + delay_ms}),
           "delayed: add delayed task") &&
       ok;
  ok =
      expect_true(!Feather_step(&feather), "delayed: not ready before delay") &&
      ok;
  g_fake_now_ms += delay_ms;
  ok = expect_true(Feather_step(&feather),
                   "delayed: executes on wake boundary") &&
       ok;
  ok = expect_true(g_counter == 1, "delayed: ran exactly once") && ok;

  Feather_deinit(&feather);
  return ok;
}

static bool run_repeat(uint64_t interval_ms, uint64_t iterations,
                       FSSchedulerTaskRepeatMode mode) {
  struct Feather feather;
  bool ok = true;
  uint64_t i;

  g_counter = 0;
  g_fake_now_ms = 1000;

  Feather_init(&feather);
  ok = expect_true(Feather_set_time_source(&feather, fake_now, NULL),
                   "repeat: set fake time source") &&
       ok;
  ok = expect_true(
           Feather_add_repeating_task(
               &feather,
               (FSSchedulerRepeatingTask){.task = count_task,
                                          .context = &g_counter,
                                          .priority = FSScheduler_Priority_UI,
                                          .execute_cycle = interval_ms,
                                          .repeat_mode = mode,
                                          .start_time = g_fake_now_ms}),
           "repeat: add periodic task") &&
       ok;

  for (i = 0; i < iterations; i++) {
    ok = expect_true(Feather_step(&feather),
                     "repeat: step executes periodic task") &&
         ok;
    g_fake_now_ms += interval_ms;
  }

  ok = expect_true(g_counter == (int)iterations,
                   "repeat: expected number of executions") &&
       ok;
  Feather_deinit(&feather);
  return ok;
}

static bool run_sleep_hint(uint64_t delay_ms) {
  struct Feather feather;
  bool ok = true;
  uint64_t sleep_ms = 0;

  g_fake_now_ms = 2000;
  Feather_init(&feather);
  ok = expect_true(Feather_set_time_source(&feather, fake_now, NULL),
                   "sleep-hint: set fake time source") &&
       ok;
  ok = expect_true(
           Feather_add_deferred_task(
               &feather,
               (FSSchedulerDeferredTask){
                   .task = count_task,
                   .context = &g_counter,
                   .priority = FSScheduler_Priority_BACKGROUND,
                   .start_time = g_fake_now_ms + delay_ms}),
           "sleep-hint: add delayed task") &&
       ok;
  ok = expect_true(Feather_next_sleep_ms(&feather, &sleep_ms) &&
                       sleep_ms == delay_ms,
                   "sleep-hint: reports remaining delay") &&
       ok;
  g_fake_now_ms += delay_ms;
  ok = expect_true(Feather_next_sleep_ms(&feather, &sleep_ms) && sleep_ms == 0,
                   "sleep-hint: returns 0 once task is due") &&
       ok;

  Feather_deinit(&feather);
  return ok;
}

static bool run_process_window(uint64_t window_ms) {
  struct Feather feather;
  bool ok = true;

  g_counter = 0;
  Feather_init(&feather);

  ok = expect_true(
           Feather_add_instant_task(
               &feather,
               (FSSchedulerInstantTask){
                   .task = count_task,
                   .context = &g_counter,
                   .priority = FSScheduler_Priority_INTERACTIVE}),
           "process-window: add immediate task") &&
       ok;
  ok = expect_true(Feather_process_for_ms(&feather, window_ms),
                   "process-window: processing API returns success") &&
       ok;
  ok = expect_true(g_counter == 1, "process-window: immediate task executed") &&
       ok;

  Feather_deinit(&feather);
  return ok;
}

static bool run_cancel(void) {
  struct Feather feather;
  uint64_t id;
  bool ok = true;

  g_counter = 0;
  g_fake_now_ms = 0;
  Feather_init(&feather);
  Feather_set_time_source(&feather, fake_now, NULL);

  /* Cancel an instant (ready) task before it runs */
  id = Feather_add_instant_task(
      &feather,
      (FSSchedulerInstantTask){.task = count_task,
                               .context = &g_counter,
                               .priority = FSScheduler_Priority_UI});
  ok = expect_true(id > 0, "cancel: instant task enqueued") && ok;
  ok = expect_true(Feather_cancel_task(&feather, id),
                   "cancel: cancel instant task succeeds") &&
       ok;
  ok = expect_true(!Feather_has_pending_tasks(&feather),
                   "cancel: no pending tasks after cancel") &&
       ok;
  ok = expect_true(!Feather_step(&feather),
                   "cancel: step returns false after cancel") &&
       ok;
  ok = expect_true(g_counter == 0, "cancel: cancelled instant task never ran") &&
       ok;

  /* Cancel a deferred (waiting) task */
  id = Feather_add_deferred_task(
      &feather,
      (FSSchedulerDeferredTask){.task = count_task,
                                .context = &g_counter,
                                .priority = FSScheduler_Priority_BACKGROUND,
                                .start_time = 1000});
  ok = expect_true(id > 0, "cancel: deferred task enqueued") && ok;
  ok = expect_true(Feather_cancel_task(&feather, id),
                   "cancel: cancel deferred task succeeds") &&
       ok;
  g_fake_now_ms = 2000;
  ok = expect_true(!Feather_step(&feather),
                   "cancel: step returns false after deferred cancel") &&
       ok;
  ok = expect_true(g_counter == 0,
                   "cancel: cancelled deferred task never ran") &&
       ok;

  /* Cancelling an unknown ID returns false */
  ok = expect_true(!Feather_cancel_task(&feather, 9999999),
                   "cancel: unknown ID returns false") &&
       ok;

  Feather_deinit(&feather);
  return ok;
}

static bool run_status(void) {
  struct Feather feather;
  uint64_t id;
  bool ok = true;

  g_counter = 0;
  g_fake_now_ms = 0;
  Feather_init(&feather);
  Feather_set_time_source(&feather, fake_now, NULL);

  /* Instant task starts PENDING_READY */
  id = Feather_add_instant_task(
      &feather,
      (FSSchedulerInstantTask){.task = count_task,
                               .context = &g_counter,
                               .priority = FSScheduler_Priority_UI});
  ok = expect_true(
           Feather_task_status(&feather, id) ==
               FSSchedulerTaskStatus_PENDING_READY,
           "status: instant task is PENDING_READY before step") &&
       ok;
  Feather_step(&feather);
  ok = expect_true(
           Feather_task_status(&feather, id) ==
               FSSchedulerTaskStatus_NOT_FOUND,
           "status: instant task is NOT_FOUND after execution") &&
       ok;

  /* Deferred task starts PENDING_WAITING, becomes PENDING_READY after promote */
  g_fake_now_ms = 0;
  id = Feather_add_deferred_task(
      &feather,
      (FSSchedulerDeferredTask){.task = count_task,
                                .context = &g_counter,
                                .priority = FSScheduler_Priority_UI,
                                .start_time = 5000});
  ok = expect_true(
           Feather_task_status(&feather, id) ==
               FSSchedulerTaskStatus_PENDING_WAITING,
           "status: deferred task is PENDING_WAITING before due") &&
       ok;
  g_fake_now_ms = 5000;
  Feather_step(&feather); /* promotes and executes */
  ok = expect_true(
           Feather_task_status(&feather, id) ==
               FSSchedulerTaskStatus_NOT_FOUND,
           "status: deferred task is NOT_FOUND after execution") &&
       ok;

  /* Unknown ID always returns NOT_FOUND */
  ok = expect_true(
           Feather_task_status(&feather, 99999) ==
               FSSchedulerTaskStatus_NOT_FOUND,
           "status: unknown ID returns NOT_FOUND") &&
       ok;

  Feather_deinit(&feather);
  return ok;
}

static bool run_state_snapshot(void) {
  struct Feather feather;
  FSSchedulerStateSnapshot state;
  bool ok = true;

  g_counter = 0;
  g_fake_now_ms = 0;
  Feather_init(&feather);
  Feather_set_time_source(&feather, fake_now, NULL);

  /* Empty scheduler */
  state = Feather_state_snapshot(&feather);
  ok = expect_true(state.total_pending == 0,
                   "state-snapshot: empty scheduler has 0 pending") &&
       ok;
  ok = expect_true(!state.has_earliest_wake_time,
                   "state-snapshot: empty scheduler has no earliest wake") &&
       ok;

  /* Add ready tasks */
  Feather_add_instant_task(
      &feather,
      (FSSchedulerInstantTask){.task = count_task,
                               .context = &g_counter,
                               .priority = FSScheduler_Priority_INTERACTIVE});
  Feather_add_instant_task(
      &feather,
      (FSSchedulerInstantTask){.task = count_task,
                               .context = &g_counter,
                               .priority = FSScheduler_Priority_UI});
  state = Feather_state_snapshot(&feather);
  ok = expect_true(state.interactive_ready_count == 1,
                   "state-snapshot: 1 interactive ready task") &&
       ok;
  ok = expect_true(state.ui_ready_count == 1,
                   "state-snapshot: 1 UI ready task") &&
       ok;
  ok = expect_true(state.total_pending == 2,
                   "state-snapshot: 2 total pending") &&
       ok;

  /* Add deferred task */
  Feather_add_deferred_task(
      &feather,
      (FSSchedulerDeferredTask){.task = count_task,
                                .context = &g_counter,
                                .priority = FSScheduler_Priority_BACKGROUND,
                                .start_time = 1000});
  state = Feather_state_snapshot(&feather);
  ok = expect_true(state.waiting_count == 1,
                   "state-snapshot: 1 waiting task") &&
       ok;
  ok = expect_true(state.paused_count == 0,
                   "state-snapshot: no paused tasks yet") &&
       ok;
  ok = expect_true(state.has_earliest_wake_time,
                   "state-snapshot: has earliest wake time") &&
       ok;
  ok = expect_true(state.earliest_wake_time_ms == 1000,
                   "state-snapshot: earliest wake time is 1000") &&
       ok;
  ok = expect_true(state.total_pending == 3,
                   "state-snapshot: 3 total pending") &&
       ok;

  /* Drain all tasks */
  g_fake_now_ms = 2000;
  while (Feather_step(&feather)) {
  }
  state = Feather_state_snapshot(&feather);
  ok = expect_true(state.total_pending == 0,
                   "state-snapshot: 0 pending after drain") &&
       ok;
  ok = expect_true(!state.has_earliest_wake_time,
                   "state-snapshot: no earliest wake after drain") &&
       ok;

  Feather_deinit(&feather);
  return ok;
}

static bool run_handle_pause_resume_reschedule(void) {
  struct Feather feather;
  FSSchedulerTaskHandler handle;
  FSSchedulerTaskStatus status;
  bool ok = true;

  g_counter = 0;
  g_fake_now_ms = 1000;
  Feather_init(&feather);
  Feather_set_time_source(&feather, fake_now, NULL);

  handle = FSScheduler_add_deferred_task_handle(
      &feather.scheduler,
      (FSSchedulerDeferredTask){.task = count_task,
                                .context = &g_counter,
                                .priority = FSScheduler_Priority_UI,
                                .start_time = 5000});
  ok = expect_true(FSScheduler_task_handle_is_valid(&handle),
                   "handle: created handle is valid") &&
       ok;
  ok = expect_true(FSScheduler_task_handle_set_user_tag(&handle, 42),
                   "handle: can set user tag") &&
       ok;
  ok = expect_true(FSScheduler_task_handle_user_tag(&handle) == 42,
                   "handle: can read user tag") &&
       ok;
  ok = expect_true(FSScheduler_task_handle_pause(&feather.scheduler, &handle),
                   "handle: pause waiting task") &&
       ok;
  status = FSScheduler_task_handle_status(&feather.scheduler, &handle);
  ok = expect_true(status == FSSchedulerTaskStatus_PENDING_PAUSED,
                   "handle: paused task reports PENDING_PAUSED") &&
       ok;
  ok = expect_true(FSScheduler_task_handle_reschedule(
                       &feather.scheduler, &handle, 2000),
                   "handle: reschedule paused task") &&
       ok;
  ok = expect_true(FSScheduler_task_handle_resume(&feather.scheduler, &handle),
                   "handle: resume paused task") &&
       ok;
  /* 1ms before rescheduled start_time (2000), the task must still wait. */
  g_fake_now_ms = 1999;
  ok = expect_true(!Feather_step(&feather),
                   "handle: resumed task waits until new start time") &&
       ok;
  g_fake_now_ms = 2000;
  ok = expect_true(Feather_step(&feather),
                   "handle: resumed task executes at rescheduled time") &&
       ok;
  ok = expect_true(g_counter == 1, "handle: resumed task ran once") && ok;

  Feather_deinit(&feather);
  return ok;
}

static bool run_deadline_timeout(void) {
  struct Feather feather;
  bool ok = true;

  g_fake_now_ms = 1000;
  g_counter = 0;
  memset(g_deadline_order, 0, sizeof(g_deadline_order));
  g_deadline_order_index = 0;
  Feather_init(&feather);
  Feather_set_time_source(&feather, fake_now, NULL);

  ok = expect_true(
           Feather_add_instant_task(
               &feather,
               (FSSchedulerInstantTask){.task = deadline_task_a,
                                        .priority = FSScheduler_Priority_UI,
                                        .deadline = 4000}) > 0,
           "deadline: enqueue task A with later deadline") &&
       ok;
  ok = expect_true(
           Feather_add_instant_task(
               &feather,
               (FSSchedulerInstantTask){.task = deadline_task_b,
                                        .priority = FSScheduler_Priority_UI,
                                        .deadline = 2000}) > 0,
           "deadline: enqueue task B with earlier deadline") &&
       ok;
  ok = expect_true(Feather_step(&feather), "deadline: first step executes") &&
       ok;
  ok = expect_true(Feather_step(&feather), "deadline: second step executes") &&
       ok;
  ok = expect_true(g_deadline_order[0] == 'B' && g_deadline_order[1] == 'A',
                   "deadline: earlier deadline wins within same priority") &&
       ok;

  {
    FSSchedulerTaskHandler timeout_handle =
        FSScheduler_add_deferred_task_handle(
            &feather.scheduler,
            (FSSchedulerDeferredTask){.task = count_task,
                                      .context = &g_counter,
                                      .priority = FSScheduler_Priority_UI,
                                      .start_time = 3000,
                                      .timeout = 1500});
    ok = expect_true(FSScheduler_task_handle_is_valid(&timeout_handle),
                     "timeout: enqueue timeout task") &&
         ok;
    g_fake_now_ms = 2500;
    ok = expect_true(!Feather_step(&feather),
                     "timeout: expired task does not execute") &&
         ok;
    ok = expect_true(FSScheduler_task_handle_status(&feather.scheduler,
                                                    &timeout_handle) ==
                         FSSchedulerTaskStatus_TIMED_OUT,
                     "timeout: status reports TIMED_OUT") &&
         ok;
  }

  Feather_deinit(&feather);
  return ok;
}

static void print_usage(const char *program) {
  printf("Usage: %s <command> [options]\n", program);
  puts("Commands:");
  puts("  once");
  puts("  delayed [--delay-ms N]");
  puts("  repeat [--interval-ms N] [--iterations N] [--mode delay|rate]");
  puts("  sleep-hint [--delay-ms N]");
  puts("  process-window [--window-ms N]");
  puts("  cancel");
  puts("  status");
  puts("  state-snapshot");
  puts("  handle");
  puts("  deadline-timeout");
  puts("  all");
}

int main(int argc, char **argv) {
  uint64_t delay_ms = 250;
  uint64_t interval_ms = 100;
  uint64_t iterations = 5;
  uint64_t window_ms = 5;
  FSSchedulerTaskRepeatMode mode = FSSchedulerTaskRepeat_FIXEDDELAY;
  bool ok = false;

  if (argc < 2) {
    print_usage(argv[0]);
    return EXIT_FAILURE;
  }

  if (!parse_u64_arg("--delay-ms", argc, argv, &delay_ms) ||
      !parse_u64_arg("--interval-ms", argc, argv, &interval_ms) ||
      !parse_u64_arg("--iterations", argc, argv, &iterations) ||
      !parse_u64_arg("--window-ms", argc, argv, &window_ms) ||
      !parse_repeat_mode_arg(argc, argv, &mode)) {
    print_usage(argv[0]);
    return EXIT_FAILURE;
  }

  if (strcmp(argv[1], "once") == 0) {
    ok = run_once();
  } else if (strcmp(argv[1], "delayed") == 0) {
    ok = run_delayed(delay_ms);
  } else if (strcmp(argv[1], "repeat") == 0) {
    ok = run_repeat(interval_ms, iterations, mode);
  } else if (strcmp(argv[1], "sleep-hint") == 0) {
    ok = run_sleep_hint(delay_ms);
  } else if (strcmp(argv[1], "process-window") == 0) {
    ok = run_process_window(window_ms);
  } else if (strcmp(argv[1], "cancel") == 0) {
    ok = run_cancel();
  } else if (strcmp(argv[1], "status") == 0) {
    ok = run_status();
  } else if (strcmp(argv[1], "state-snapshot") == 0) {
    ok = run_state_snapshot();
  } else if (strcmp(argv[1], "handle") == 0) {
    ok = run_handle_pause_resume_reschedule();
  } else if (strcmp(argv[1], "deadline-timeout") == 0) {
    ok = run_deadline_timeout();
  } else if (strcmp(argv[1], "all") == 0) {
    ok = run_once();
    ok = run_delayed(delay_ms) && ok;
    ok = run_repeat(interval_ms, iterations, FSSchedulerTaskRepeat_FIXEDDELAY) && ok;
    ok = run_repeat(interval_ms, iterations, FSSchedulerTaskRepeat_FIXEDRATE) && ok;
    ok = run_sleep_hint(delay_ms) && ok;
    ok = run_process_window(window_ms) && ok;
    ok = run_cancel() && ok;
    ok = run_status() && ok;
    ok = run_state_snapshot() && ok;
    ok = run_handle_pause_resume_reschedule() && ok;
    ok = run_deadline_timeout() && ok;
  } else {
    print_usage(argv[0]);
    return EXIT_FAILURE;
  }

  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
