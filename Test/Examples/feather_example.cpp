#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "Feather.hpp"

static int immediate_count = 0;
static int delayed_count = 0;
static int repeat_count = 0;
static int fixed_rate_count = 0;
static int fixed_delay_count = 0;
static int process_window_count = 0;
static char starvation_log[16];
static int starvation_index = 0;
static uint64_t fake_now_ms = 0;

static void run_immediate(void *context) {
  (void)context;
  immediate_count++;
}

static void run_delayed(void *context) {
  (void)context;
  delayed_count++;
}

static void run_repeat(void *context) {
  (void)context;
  repeat_count++;
}

static void run_repeat_fixed_rate(void *context) {
  int *counter = (int *)context;
  (*counter)++;
}

static void run_repeat_fixed_delay(void *context) {
  int *counter = (int *)context;
  (*counter)++;
  fake_now_ms += 50;
}

static void run_process_window_task(void *context) {
  (void)context;
  process_window_count++;
}

static void run_interactive(void *context) {
  (void)context;
  starvation_log[starvation_index++] = 'I';
}

static void run_bg(void *context) {
  (void)context;
  starvation_log[starvation_index++] = 'B';
}

static uint64_t fake_now(void *context) {
  (void)context;
  return fake_now_ms;
}

static void sleep_ms(long milliseconds) {
  struct timespec duration;

  duration.tv_sec = milliseconds / 1000L;
  duration.tv_nsec = (milliseconds % 1000L) * 1000000L;
  nanosleep(&duration, NULL);
}

static bool expect_true(bool condition, const char *message) {
  if (!condition) {
    printf("FAIL: %s\n", message);
    return false;
  }

  printf("PASS: %s\n", message);
  return true;
}

int main(void) {
  struct Feather feather;
  bool success = true;
  uint64_t now_ms;

  Feather_init(&feather);
  now_ms = FSScheduler_now_ms();
  Feather_add_deferred_task(
      &feather,
      (FSSchedulerDeferredTask){.task = run_delayed,
                                .context = NULL,
                                .priority = FSScheduler_Priority_BACKGROUND,
                                .start_time = now_ms + 100});
  Feather_add_instant_task(
      &feather, (FSSchedulerInstantTask){
                    .task = run_immediate,
                    .priority = FSScheduler_Priority_BACKGROUND});
  success = expect_true(Feather_step(&feather),
                        "time-based: immediate task runs first") &&
            success;
  success =
      expect_true(
          immediate_count == 1 && delayed_count == 0,
          "time-based: future task does not block ready task in same queue") &&
      success;
  success = expect_true(!Feather_step(&feather),
                        "time-based: no ready task before delay elapses") &&
            success;
  success =
      expect_true(Feather_has_pending_tasks(&feather),
                  "time-based: delayed task remains pending while not ready") &&
      success;
  sleep_ms(150);
  success = expect_true(Feather_step(&feather),
                        "time-based: delayed task runs after start_time") &&
            success;
  success = expect_true(delayed_count == 1,
                        "time-based: delayed task executed exactly once") &&
            success;

  Feather_deinit(&feather);
  Feather_init(&feather);
  Feather_add_repeating_task(
      &feather, (FSSchedulerRepeatingTask){.task = run_repeat,
                                           .priority = FSScheduler_Priority_UI,
                                           .execute_cycle = 100});
  success = expect_true(Feather_step(&feather),
                        "repeat: first execution runs immediately") &&
            success;
  success = expect_true(repeat_count == 1, "repeat: first execution counted") &&
            success;
  success = expect_true(!Feather_step(&feather),
                        "repeat: task waits until next cycle") &&
            success;
  sleep_ms(150);
  success = expect_true(Feather_step(&feather),
                        "repeat: task runs again after execute_cycle") &&
            success;
  success =
      expect_true(repeat_count == 2, "repeat: second execution counted") &&
      success;

  Feather_deinit(&feather);
  Feather_init(&feather);
  success = expect_true(Feather_set_time_source(&feather, fake_now, NULL),
                        "time source: custom monotonic source accepted") &&
            success;
  fake_now_ms = 1000;
  Feather_add_repeating_task(
      &feather,
      (FSSchedulerRepeatingTask){.task = run_repeat_fixed_rate,
                                  .context = &fixed_rate_count,
                                  .priority = FSScheduler_Priority_UI,
                                  .start_time = fake_now_ms,
                                  .execute_cycle = 100,
                                  .repeat_mode = FSSchedulerTaskRepeat_FIXEDRATE});
  success = expect_true(Feather_step(&feather),
                        "fixed-rate: first execution runs at scheduled time") &&
            success;
  success = expect_true(fixed_rate_count == 1,
                        "fixed-rate: first execution counted") &&
            success;
  fake_now_ms = 1350;
  success = expect_true(Feather_step(&feather),
                        "fixed-rate: overdue periodic task executes once") &&
            success;
  success = expect_true(fixed_rate_count == 2,
                        "fixed-rate: second execution counted") &&
            success;
  fake_now_ms = 1399;
  success =
      expect_true(!Feather_step(&feather),
                  "fixed-rate: task catches up to next future boundary") &&
      success;
  success = expect_true(Feather_has_pending_tasks(&feather),
                        "fixed-rate: periodic task remains pending while "
                        "waiting for next boundary") &&
            success;
  fake_now_ms = 1400;
  success = expect_true(Feather_step(&feather),
                        "fixed-rate: runs again exactly on boundary") &&
            success;
  success = expect_true(fixed_rate_count == 3,
                        "fixed-rate: third execution counted") &&
            success;

  Feather_deinit(&feather);
  Feather_init(&feather);
  success = expect_true(Feather_set_time_source(&feather, fake_now, NULL),
                        "fixed-delay: custom monotonic source accepted") &&
            success;
  fixed_delay_count = 0;
  fake_now_ms = 1000;
  Feather_add_repeating_task(
      &feather,
      (FSSchedulerRepeatingTask){.task = run_repeat_fixed_delay,
                                  .context = &fixed_delay_count,
                                  .priority = FSScheduler_Priority_UI,
                                  .start_time = fake_now_ms,
                                  .execute_cycle = 100,
                                  .repeat_mode = FSSchedulerTaskRepeat_FIXEDDELAY});
  success =
      expect_true(Feather_step(&feather),
                  "fixed-delay: first execution runs at scheduled time") &&
      success;
  success = expect_true(fixed_delay_count == 1,
                        "fixed-delay: first execution counted") &&
            success;
  fake_now_ms = 1149;
  success =
      expect_true(!Feather_step(&feather),
                  "fixed-delay: reschedule is based on completion time") &&
      success;
  fake_now_ms = 1150;
  success =
      expect_true(Feather_step(&feather),
                  "fixed-delay: task runs on completion-based boundary") &&
      success;
  success = expect_true(fixed_delay_count == 2,
                        "fixed-delay: second execution counted") &&
            success;

  Feather_deinit(&feather);
  Feather_init(&feather);
  success = expect_true(Feather_set_time_source(&feather, fake_now, NULL),
                        "sleep-hint: custom monotonic source accepted") &&
            success;
  fake_now_ms = 2000;
  Feather_add_deferred_task(
      &feather,
      (FSSchedulerDeferredTask){.task = run_delayed,
                                .priority = FSScheduler_Priority_UI,
                                .start_time = fake_now_ms + 250,
                                });
  {
    uint64_t sleep_ms = 0;
    success = expect_true(
                  Feather_next_sleep_ms(&feather, &sleep_ms) && sleep_ms == 250,
                  "sleep-hint: returns time until earliest delayed task") &&
              success;
    fake_now_ms = 2250;
    success =
        expect_true(Feather_next_sleep_ms(&feather, &sleep_ms) && sleep_ms == 0,
                    "sleep-hint: returns zero when delayed task is ready") &&
        success;
  }

  Feather_deinit(&feather);
  Feather_init(&feather);
  process_window_count = 0;
  Feather_add_instant_task(
      &feather, (FSSchedulerInstantTask){.task = run_process_window_task,
                                         .priority = FSScheduler_Priority_UI});
  success =
      expect_true(Feather_process_for_ms(&feather, 5),
                  "process-window: scheduler runs for requested time budget") &&
      success;
  success =
      expect_true(
          process_window_count == 1,
          "process-window: immediate task executed inside processing window") &&
      success;

  Feather_deinit(&feather);
  Feather_init(&feather);
  memset(starvation_log, 0, sizeof(starvation_log));
  starvation_index = 0;
  Feather_add_instant_task(
      &feather,
      (FSSchedulerInstantTask){.task = run_bg,
                                .priority = FSScheduler_Priority_BACKGROUND});
  Feather_add_instant_task(
      &feather,
      (FSSchedulerInstantTask){.task = run_interactive,
                                .priority = FSScheduler_Priority_INTERACTIVE});
  Feather_add_instant_task(
      &feather,
      (FSSchedulerInstantTask){.task = run_interactive,
                                .priority = FSScheduler_Priority_INTERACTIVE});
  Feather_add_instant_task(
      &feather,
      (FSSchedulerInstantTask){.task = run_interactive,
                                .priority = FSScheduler_Priority_INTERACTIVE});
  Feather_add_instant_task(
      &feather,
      (FSSchedulerInstantTask){.task = run_interactive,
                                .priority = FSScheduler_Priority_INTERACTIVE});
  Feather_add_instant_task(
      &feather,
      (FSSchedulerInstantTask){.task = run_interactive,
                                .priority = FSScheduler_Priority_INTERACTIVE});
  Feather_step(&feather);
  Feather_step(&feather);
  Feather_step(&feather);
  Feather_step(&feather);
  Feather_step(&feather);
  success = expect_true(strcmp(starvation_log, "IIIIB") == 0,
                        "starvation: background task runs before interactive "
                        "queue drains completely") &&
            success;

  Feather_deinit(&feather);
  if (!success) {
    return EXIT_FAILURE;
  }

  puts("All scheduler tests passed.");
  return EXIT_SUCCESS;
}
