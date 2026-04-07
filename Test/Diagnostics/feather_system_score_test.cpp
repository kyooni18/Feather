#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/task_info.h>
#endif

#include "Feather.hpp"
#include "FeatherRuntime/FSAllocator.hpp"
#include "FeatherRuntime/FSTime.hpp"

#if defined(FEATHER_SYSTEM_SCORE_HAS_RESOURCE_TRACKING)
#include "FeatherRuntime/FSResourceTracker.hpp"
#endif

#define MAX_EVENT_LOG 64
#define COUNTING_ALLOCATOR_MAX_RECORDS 4096
#define ENDURANCE_TICK_MS 5ULL
#define ENDURANCE_SAFETY_STEPS 256
#define DEFAULT_DURATION_MS 20000ULL
#define DEFAULT_PROCESS_WINDOW_MS 180ULL

typedef struct Scoreboard {
  int assertions;
  int failures;
  int suites_run;
  int suites_failed;
  double flow_earned;
  double flow_possible;
  double device_earned;
  double device_possible;
} Scoreboard;

typedef struct SuiteContext {
  Scoreboard *scoreboard;
  const char *name;
  int assertions;
  int failures;
  double device_earned;
  double device_possible;
} SuiteContext;

typedef struct RuntimeConfig {
  uint64_t duration_ms;
  uint64_t process_window_ms;
} RuntimeConfig;

typedef struct ProcessSnapshot {
  double wall_seconds;
  double cpu_seconds;
  long rss_kb;
  long hwm_kb;
  long voluntary_ctxt;
  long involuntary_ctxt;
} ProcessSnapshot;

typedef struct FakeClock {
  uint64_t now_ms;
} FakeClock;

typedef struct EventRecorder {
  char events[MAX_EVENT_LOG];
  size_t count;
} EventRecorder;

typedef struct LogTaskContext {
  EventRecorder *recorder;
  char token;
} LogTaskContext;

typedef struct CounterTaskContext {
  int *counter;
} CounterTaskContext;

typedef struct AdvanceCounterTaskContext {
  int *counter;
  FakeClock *clock;
  uint64_t advance_ms;
} AdvanceCounterTaskContext;

typedef struct CountingAllocatorRecord {
  void *pointer;
  size_t size;
} CountingAllocatorRecord;

typedef struct CountingAllocator {
  const FSAllocator *base;
  FSAllocator allocator;
  CountingAllocatorRecord records[COUNTING_ALLOCATOR_MAX_RECORDS];
  size_t record_count;
  size_t current_bytes;
  size_t peak_bytes;
  size_t total_allocated_bytes;
  size_t total_freed_bytes;
  uint64_t allocate_calls;
  uint64_t reallocate_calls;
  uint64_t deallocate_calls;
} CountingAllocator;

typedef struct EnduranceHarness {
  struct Feather feather;
  FakeClock clock;
#if defined(FEATHER_SYSTEM_SCORE_HAS_RESOURCE_TRACKING)
  FSResourceTracker tracking;
#endif
  int interactive_runs;
  int ui_runs;
  int background_runs;
  int injector_runs;
  int child_runs;
  int delayed_probe_runs;
  int enqueue_failures;
  int pending_peak;
  int total_executions;
  uint64_t steps;
  uint64_t ticks;
} EnduranceHarness;

static uint64_t fake_clock_now(void *context) {
  FakeClock *clock = (FakeClock *)context;

  return (clock != NULL) ? clock->now_ms : 0;
}

static double monotonic_seconds(void) {
  struct timespec now;

  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
    return 0.0;
  }

  return (double)now.tv_sec + ((double)now.tv_nsec / 1000000000.0);
}

static double process_cpu_seconds(void) {
  struct timespec now;

  if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &now) != 0) {
    return 0.0;
  }

  return (double)now.tv_sec + ((double)now.tv_nsec / 1000000000.0);
}

static long bytes_to_kb(uint64_t bytes) { return (long)(bytes / 1024ULL); }

static double clamp_unit(double value) {
  if (value < 0.0) {
    return 0.0;
  }
  if (value > 1.0) {
    return 1.0;
  }
  return value;
}

static double clamp_score(double value, double max_value) {
  if (value < 0.0) {
    return 0.0;
  }
  if (value > max_value) {
    return max_value;
  }
  return value;
}

static double absolute_double(double value) {
  return (value < 0.0) ? -value : value;
}

static void capture_process_snapshot(ProcessSnapshot *snapshot) {
  struct rusage usage;

  if (snapshot == NULL) {
    return;
  }

  memset(snapshot, 0, sizeof(*snapshot));
  snapshot->wall_seconds = monotonic_seconds();
  snapshot->cpu_seconds = process_cpu_seconds();
  snapshot->rss_kb = -1;
  snapshot->hwm_kb = -1;

#if defined(__APPLE__)
  {
    mach_task_basic_info_data_t info;
    mach_msg_type_number_t info_count = MACH_TASK_BASIC_INFO_COUNT;

    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info,
                  &info_count) == KERN_SUCCESS) {
      snapshot->rss_kb = bytes_to_kb(info.resident_size);
      snapshot->hwm_kb = bytes_to_kb(info.resident_size_max);
    }
  }
#else
  {
    FILE *fp = fopen("/proc/self/status", "r");
    char line[256];

    if (fp != NULL) {
      while (fgets(line, sizeof(line), fp) != NULL) {
        if (strncmp(line, "VmRSS:", 6) == 0) {
          snapshot->rss_kb = strtol(line + 6, NULL, 10);
        } else if (strncmp(line, "VmHWM:", 6) == 0) {
          snapshot->hwm_kb = strtol(line + 6, NULL, 10);
        }
      }
      fclose(fp);
    }
  }
#endif

  if (getrusage(RUSAGE_SELF, &usage) == 0) {
    snapshot->voluntary_ctxt = usage.ru_nvcsw;
    snapshot->involuntary_ctxt = usage.ru_nivcsw;
  } else {
    snapshot->voluntary_ctxt = -1;
    snapshot->involuntary_ctxt = -1;
  }
}

static double process_snapshot_cpu_percent(const ProcessSnapshot *before,
                                           const ProcessSnapshot *after) {
  double wall_delta;
  double cpu_delta;

  if (before == NULL || after == NULL) {
    return 0.0;
  }

  wall_delta = after->wall_seconds - before->wall_seconds;
  cpu_delta = after->cpu_seconds - before->cpu_seconds;
  if (wall_delta <= 0.0) {
    return 0.0;
  }

  return (cpu_delta / wall_delta) * 100.0;
}

static void suite_begin(SuiteContext *suite, Scoreboard *scoreboard,
                        const char *name) {
  if (suite == NULL) {
    return;
  }

  suite->scoreboard = scoreboard;
  suite->name = name;
  suite->assertions = 0;
  suite->failures = 0;
  suite->device_earned = 0.0;
  suite->device_possible = 0.0;
  printf("\n=== %s ===\n", name);
}

static void suite_expect(SuiteContext *suite, bool condition,
                         const char *message) {
  if (suite == NULL || suite->scoreboard == NULL) {
    return;
  }

  suite->assertions++;
  suite->scoreboard->assertions++;

  if (!condition) {
    suite->failures++;
    suite->scoreboard->failures++;
    printf("  [FAIL] %s\n", message);
    return;
  }

  printf("  [PASS] %s\n", message);
}

static void suite_add_device_metric(SuiteContext *suite, const char *label,
                                    double earned, double possible,
                                    double observed, const char *unit) {
  if (suite == NULL) {
    return;
  }

  suite->device_earned += clamp_score(earned, possible);
  suite->device_possible += possible;
  printf("  [metric] %s = %.2f%s (%.1f/%.1f)\n", label, observed,
         (unit != NULL) ? unit : "", clamp_score(earned, possible), possible);
}

static void suite_finish(SuiteContext *suite, double flow_weight) {
  double pass_ratio;
  double flow_earned;

  if (suite == NULL || suite->scoreboard == NULL) {
    return;
  }

  suite->scoreboard->suites_run++;
  if (suite->failures > 0) {
    suite->scoreboard->suites_failed++;
  }

  if (suite->assertions == 0) {
    pass_ratio = 1.0;
  } else {
    pass_ratio =
        (double)(suite->assertions - suite->failures) / (double)suite->assertions;
  }

  flow_earned = flow_weight * pass_ratio;
  suite->scoreboard->flow_earned += flow_earned;
  suite->scoreboard->flow_possible += flow_weight;
  suite->scoreboard->device_earned += suite->device_earned;
  suite->scoreboard->device_possible += suite->device_possible;

  printf("  [suite] assertions=%d failures=%d flow=%.1f/%.1f device=%.1f/%.1f\n",
         suite->assertions, suite->failures, flow_earned, flow_weight,
         suite->device_earned, suite->device_possible);
}

static void event_recorder_reset(EventRecorder *recorder) {
  if (recorder == NULL) {
    return;
  }

  memset(recorder->events, 0, sizeof(recorder->events));
  recorder->count = 0;
}

static void record_token_task(void *context) {
  LogTaskContext *task_context = (LogTaskContext *)context;

  if (task_context == NULL || task_context->recorder == NULL ||
      task_context->recorder->count >= (sizeof(task_context->recorder->events) - 1U)) {
    return;
  }

  task_context->recorder->events[task_context->recorder->count++] =
      task_context->token;
  task_context->recorder->events[task_context->recorder->count] = '\0';
}

static void count_task(void *context) {
  CounterTaskContext *task_context = (CounterTaskContext *)context;

  if (task_context != NULL && task_context->counter != NULL) {
    (*task_context->counter)++;
  }
}

static void count_and_advance_task(void *context) {
  AdvanceCounterTaskContext *task_context = (AdvanceCounterTaskContext *)context;

  if (task_context == NULL) {
    return;
  }

  if (task_context->counter != NULL) {
    (*task_context->counter)++;
  }

  if (task_context->clock != NULL) {
    task_context->clock->now_ms += task_context->advance_ms;
  }
}

static size_t counting_allocator_find(const CountingAllocator *allocator,
                                      void *pointer) {
  size_t i;

  if (allocator == NULL || pointer == NULL) {
    return COUNTING_ALLOCATOR_MAX_RECORDS;
  }

  for (i = 0; i < allocator->record_count; i++) {
    if (allocator->records[i].pointer == pointer) {
      return i;
    }
  }

  return COUNTING_ALLOCATOR_MAX_RECORDS;
}

static void counting_allocator_track(CountingAllocator *allocator, void *pointer,
                                     size_t size) {
  if (allocator == NULL || pointer == NULL || size == 0 ||
      allocator->record_count >= COUNTING_ALLOCATOR_MAX_RECORDS) {
    return;
  }

  allocator->records[allocator->record_count].pointer = pointer;
  allocator->records[allocator->record_count].size = size;
  allocator->record_count++;
  allocator->current_bytes += size;
  allocator->total_allocated_bytes += size;

  if (allocator->current_bytes > allocator->peak_bytes) {
    allocator->peak_bytes = allocator->current_bytes;
  }
}

static void counting_allocator_untrack(CountingAllocator *allocator,
                                       size_t record_index) {
  size_t size;

  if (allocator == NULL || record_index >= allocator->record_count) {
    return;
  }

  size = allocator->records[record_index].size;
  allocator->current_bytes -= size;
  allocator->total_freed_bytes += size;
  allocator->record_count--;

  if (record_index < allocator->record_count) {
    allocator->records[record_index] =
        allocator->records[allocator->record_count];
  }
}

static void *counting_allocator_allocate_impl(void *context, size_t size) {
  CountingAllocator *allocator = (CountingAllocator *)context;
  void *pointer;

  if (allocator == NULL || size == 0) {
    return NULL;
  }

  allocator->allocate_calls++;
  pointer = FSAllocator_allocate(allocator->base, size);
  if (pointer == NULL) {
    return NULL;
  }

  if (allocator->record_count >= COUNTING_ALLOCATOR_MAX_RECORDS) {
    FSAllocator_deallocate(allocator->base, pointer);
    return NULL;
  }

  counting_allocator_track(allocator, pointer, size);
  return pointer;
}

static void *counting_allocator_reallocate_impl(void *context, void *pointer,
                                                size_t size) {
  CountingAllocator *allocator = (CountingAllocator *)context;
  size_t record_index;
  size_t previous_size = 0;
  void *new_pointer;

  if (allocator == NULL) {
    return NULL;
  }

  allocator->reallocate_calls++;

  if (pointer == NULL) {
    return counting_allocator_allocate_impl(context, size);
  }

  if (size == 0) {
    record_index = counting_allocator_find(allocator, pointer);
    if (record_index < COUNTING_ALLOCATOR_MAX_RECORDS) {
      counting_allocator_untrack(allocator, record_index);
    }
    FSAllocator_deallocate(allocator->base, pointer);
    return NULL;
  }

  record_index = counting_allocator_find(allocator, pointer);
  if (record_index < COUNTING_ALLOCATOR_MAX_RECORDS) {
    previous_size = allocator->records[record_index].size;
  }

  new_pointer = FSAllocator_reallocate(allocator->base, pointer, size);
  if (new_pointer == NULL) {
    return NULL;
  }

  if (record_index >= COUNTING_ALLOCATOR_MAX_RECORDS) {
    if (allocator->record_count >= COUNTING_ALLOCATOR_MAX_RECORDS) {
      FSAllocator_deallocate(allocator->base, new_pointer);
      return NULL;
    }
    counting_allocator_track(allocator, new_pointer, size);
    return new_pointer;
  }

  allocator->records[record_index].pointer = new_pointer;
  allocator->records[record_index].size = size;

  if (size > previous_size) {
    size_t delta = size - previous_size;
    allocator->current_bytes += delta;
    allocator->total_allocated_bytes += delta;
  } else if (previous_size > size) {
    size_t delta = previous_size - size;
    allocator->current_bytes -= delta;
    allocator->total_freed_bytes += delta;
  }

  if (allocator->current_bytes > allocator->peak_bytes) {
    allocator->peak_bytes = allocator->current_bytes;
  }

  return new_pointer;
}

static void counting_allocator_deallocate_impl(void *context, void *pointer) {
  CountingAllocator *allocator = (CountingAllocator *)context;
  size_t record_index;

  if (allocator == NULL || pointer == NULL) {
    return;
  }

  allocator->deallocate_calls++;
  record_index = counting_allocator_find(allocator, pointer);
  if (record_index < COUNTING_ALLOCATOR_MAX_RECORDS) {
    counting_allocator_untrack(allocator, record_index);
  }

  FSAllocator_deallocate(allocator->base, pointer);
}

static void counting_allocator_init(CountingAllocator *allocator) {
  if (allocator == NULL) {
    return;
  }

  memset(allocator, 0, sizeof(*allocator));
  allocator->base = FSAllocator_resolve(NULL);
  allocator->allocator = (FSAllocator){
      .context = allocator,
      .allocate = counting_allocator_allocate_impl,
      .reallocate = counting_allocator_reallocate_impl,
      .deallocate = counting_allocator_deallocate_impl};
}

static void endurance_update_pending_peak(EnduranceHarness *harness) {
  int pending;

  if (harness == NULL) {
    return;
  }

  pending = harness->feather.scheduler.bgCount + harness->feather.scheduler.uiCount +
            harness->feather.scheduler.interactiveCount;
  if (pending > harness->pending_peak) {
    harness->pending_peak = pending;
  }
}

static bool endurance_add_instant_task(EnduranceHarness *harness,
                                       FSSchedulerInstantTask task) {
  if (harness == NULL) {
    return false;
  }

  if (!Feather_add_instant_task(&harness->feather, task)) {
    harness->enqueue_failures++;
    return false;
  }

  endurance_update_pending_peak(harness);
  return true;
}

static bool endurance_add_deferred_task(EnduranceHarness *harness,
                                        FSSchedulerDeferredTask task) {
  if (harness == NULL) {
    return false;
  }

  if (!Feather_add_deferred_task(&harness->feather, task)) {
    harness->enqueue_failures++;
    return false;
  }

  endurance_update_pending_peak(harness);
  return true;
}

static bool endurance_add_repeating_task(EnduranceHarness *harness,
                                         FSSchedulerRepeatingTask task) {
  if (harness == NULL) {
    return false;
  }

  if (!Feather_add_repeating_task(&harness->feather, task)) {
    harness->enqueue_failures++;
    return false;
  }

  endurance_update_pending_peak(harness);
  return true;
}

static void endurance_child_task(void *context) {
  EnduranceHarness *harness = (EnduranceHarness *)context;

  if (harness == NULL) {
    return;
  }

  harness->child_runs++;
  harness->total_executions++;
}

static void endurance_delayed_probe_task(void *context) {
  EnduranceHarness *harness = (EnduranceHarness *)context;

  if (harness == NULL) {
    return;
  }

  harness->delayed_probe_runs++;
  harness->total_executions++;
}

static void endurance_interactive_task(void *context) {
  EnduranceHarness *harness = (EnduranceHarness *)context;

  if (harness == NULL) {
    return;
  }

  harness->interactive_runs++;
  harness->total_executions++;

  if ((harness->interactive_runs % 7) == 0) {
    (void)endurance_add_deferred_task(
        harness,
        (FSSchedulerDeferredTask){.task = endurance_child_task,
                                  .context = harness,
                                  .priority = FSScheduler_Priority_UI,
                                  .start_time = harness->clock.now_ms + 3});
  }
}

static void endurance_ui_task(void *context) {
  EnduranceHarness *harness = (EnduranceHarness *)context;

  if (harness == NULL) {
    return;
  }

  harness->ui_runs++;
  harness->total_executions++;

  if ((harness->ui_runs % 5) == 0) {
    (void)endurance_add_instant_task(
        harness,
        (FSSchedulerInstantTask){.task = endurance_child_task,
                                  .context = harness,
                                  .priority = FSScheduler_Priority_BACKGROUND});
  }
}

static void endurance_background_task(void *context) {
  EnduranceHarness *harness = (EnduranceHarness *)context;

  if (harness == NULL) {
    return;
  }

  harness->background_runs++;
  harness->total_executions++;

  if ((harness->background_runs % 4) == 0) {
    (void)endurance_add_deferred_task(
        harness,
        (FSSchedulerDeferredTask){.task = endurance_child_task,
                                  .context = harness,
                                  .priority = FSScheduler_Priority_INTERACTIVE,
                                  .start_time = harness->clock.now_ms + 2});
  }
}

static void endurance_injector_task(void *context) {
  EnduranceHarness *harness = (EnduranceHarness *)context;

  if (harness == NULL) {
    return;
  }

  harness->injector_runs++;
  harness->total_executions++;

  (void)endurance_add_instant_task(
      harness,
      (FSSchedulerInstantTask){.task = endurance_child_task,
                                .context = harness,
                                .priority = FSScheduler_Priority_INTERACTIVE});
  (void)endurance_add_deferred_task(
      harness,
      (FSSchedulerDeferredTask){.task = endurance_delayed_probe_task,
                                .context = harness,
                                .priority = FSScheduler_Priority_UI,
                                .start_time = harness->clock.now_ms + 2});

  if ((harness->injector_runs % 2) == 0) {
    (void)endurance_add_deferred_task(
        harness,
        (FSSchedulerDeferredTask){.task = endurance_child_task,
                                  .context = harness,
                                  .priority = FSScheduler_Priority_BACKGROUND,
                                  .start_time = harness->clock.now_ms + 4});
  }
}

static bool endurance_seed(EnduranceHarness *harness) {
  if (harness == NULL) {
    return false;
  }

  return endurance_add_repeating_task(
             harness,
             (FSSchedulerRepeatingTask){
                 .task = endurance_interactive_task,
                 .context = harness,
                 .priority = FSScheduler_Priority_INTERACTIVE,
                 .execute_cycle = 5,
                 .repeat_mode = FSSchedulerTaskRepeat_FIXEDRATE,
                 .start_time = harness->clock.now_ms}) &&
         endurance_add_repeating_task(
             harness,
             (FSSchedulerRepeatingTask){
                 .task = endurance_ui_task,
                 .context = harness,
                 .priority = FSScheduler_Priority_UI,
                 .execute_cycle = 9,
                 .repeat_mode = FSSchedulerTaskRepeat_FIXEDDELAY,
                 .start_time = harness->clock.now_ms}) &&
         endurance_add_repeating_task(
             harness,
             (FSSchedulerRepeatingTask){
                 .task = endurance_background_task,
                 .context = harness,
                 .priority = FSScheduler_Priority_BACKGROUND,
                 .execute_cycle = 17,
                 .repeat_mode = FSSchedulerTaskRepeat_FIXEDDELAY,
                 .start_time = harness->clock.now_ms}) &&
         endurance_add_repeating_task(
             harness,
             (FSSchedulerRepeatingTask){
                 .task = endurance_injector_task,
                 .context = harness,
                 .priority = FSScheduler_Priority_UI,
                 .execute_cycle = 31,
                 .repeat_mode = FSSchedulerTaskRepeat_FIXEDDELAY,
                 .start_time = harness->clock.now_ms + 1}) &&
         endurance_add_deferred_task(
             harness,
             (FSSchedulerDeferredTask){
                 .task = endurance_delayed_probe_task,
                 .context = harness,
                 .priority = FSScheduler_Priority_BACKGROUND,
                 .start_time = harness->clock.now_ms + 11});
}

static bool run_endurance_workload(EnduranceHarness *harness,
                                   uint64_t duration_ms) {
  uint64_t stop_time;

  if (harness == NULL) {
    return false;
  }

  stop_time = harness->clock.now_ms + duration_ms;
  while (harness->clock.now_ms < stop_time) {
    uint64_t steps_this_tick = 0;

    while (Feather_step(&harness->feather)) {
      harness->steps++;
      steps_this_tick++;
      endurance_update_pending_peak(harness);
      if (steps_this_tick >= ENDURANCE_SAFETY_STEPS) {
        return false;
      }
    }

    harness->ticks++;
    harness->clock.now_ms += ENDURANCE_TICK_MS;
  }

  {
    uint64_t drain_steps = 0;

    while (Feather_step(&harness->feather)) {
      harness->steps++;
      drain_steps++;
      endurance_update_pending_peak(harness);
      if (drain_steps >= ENDURANCE_SAFETY_STEPS) {
        return false;
      }
    }
  }

  return true;
}

static bool parse_u64_flag(int argc, char **argv, const char *flag,
                           uint64_t *value) {
  int i;

  if (value == NULL) {
    return false;
  }

  for (i = 1; i < argc - 1; i++) {
    if (strcmp(argv[i], flag) == 0) {
      char *end = NULL;
      unsigned long long parsed = strtoull(argv[i + 1], &end, 10);

      if (end == argv[i + 1] || *end != '\0') {
        return false;
      }

      *value = (uint64_t)parsed;
    }
  }

  return true;
}

static bool parse_args(int argc, char **argv, RuntimeConfig *config) {
  int i;

  if (config == NULL) {
    return false;
  }

  config->duration_ms = DEFAULT_DURATION_MS;
  config->process_window_ms = DEFAULT_PROCESS_WINDOW_MS;

  for (i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--help") == 0) {
      return false;
    }

    if (strcmp(argv[i], "--duration-ms") == 0 ||
        strcmp(argv[i], "--process-window-ms") == 0) {
      i++;
      continue;
    }

    return false;
  }

  return parse_u64_flag(argc, argv, "--duration-ms", &config->duration_ms) &&
         parse_u64_flag(argc, argv, "--process-window-ms",
                        &config->process_window_ms) &&
         config->duration_ms > 0 && config->process_window_ms > 0;
}

static void print_usage(const char *program) {
  printf("Usage: %s [--duration-ms N] [--process-window-ms N]\n", program);
  puts("  --duration-ms        simulated endurance duration (default 20000)");
  puts("  --process-window-ms  real-time process_for_ms probe window (default 180)");
}

static void run_api_surface_suite(Scoreboard *scoreboard) {
  SuiteContext suite;
  FakeClock clock = {.now_ms = 5000};
  CountingAllocator allocator;
  struct Feather feather;
  CounterTaskContext counter_context;
  uint64_t before_sleep_ms;
  uint64_t after_sleep_ms;
  int counter = 0;

  counting_allocator_init(&allocator);
  suite_begin(&suite, scoreboard, "API Surface");

  suite_expect(&suite, FSAllocator_resolve(NULL) != NULL,
               "allocator: resolve(NULL) yields a usable allocator");
  suite_expect(&suite, FSTime_init.now_monotonic_ms != NULL &&
                           FSTime_init.now_unix_ms != NULL &&
                           FSTime_init.sleep_ms != NULL,
               "time: function-table entrypoints are wired");

  before_sleep_ms = FSTime_now_monotonic();
  suite_expect(&suite, FSTime_sleep_ms(1), "time: 1 ms sleep succeeds");
  after_sleep_ms = FSTime_now_monotonic();
  suite_expect(&suite, after_sleep_ms >= before_sleep_ms,
               "time: monotonic clock does not move backwards");
  suite_expect(&suite, FSTime_now_unix() > 0,
               "time: UNIX wall clock is readable");

  suite_expect(&suite,
               Feather_init_with_config(
                   &feather, &(FeatherConfig){.allocator = &allocator.allocator,
                                              .now_fn = fake_clock_now,
                                              .now_context = &clock}),
               "feather: init_with_config accepts custom allocator and clock");
  suite_expect(&suite,
               feather.scheduler.now_fn == fake_clock_now,
               "feather: scheduler clock is installed");

  counter_context.counter = &counter;
  suite_expect(&suite,
               Feather_add_instant_task(
                   &feather,
                   (FSSchedulerInstantTask){.task = count_task,
                                            .context = &counter_context,
                                            .priority = FSScheduler_Priority_UI}),
               "feather: add_task works through wrapper surface");
  suite_expect(&suite, Feather_step(&feather) && counter == 1,
               "feather: step executes enqueued task");

  Feather_deinit(&feather);
  suite_expect(&suite,
               feather.scheduler.now_fn == NULL,
               "feather: deinit clears scheduler clock");

  suite_finish(&suite, 12.0);
}

static void run_allocator_suite(Scoreboard *scoreboard) {
  SuiteContext suite;
  CountingAllocator allocator;
  struct Feather feather;
  FakeClock clock = {.now_ms = 1000};
  void *pointer;
  int i;

  counting_allocator_init(&allocator);
  suite_begin(&suite, scoreboard, "Allocator Path");

  pointer = FSAllocator_allocate(&allocator.allocator, 64);
  suite_expect(&suite, pointer != NULL, "allocator: custom allocate succeeds");
  pointer = FSAllocator_reallocate(&allocator.allocator, pointer, 192);
  suite_expect(&suite, pointer != NULL, "allocator: custom reallocate succeeds");
  FSAllocator_deallocate(&allocator.allocator, pointer);
  suite_expect(&suite, allocator.current_bytes == 0,
               "allocator: direct allocations fully release tracked bytes");

  suite_expect(&suite,
               Feather_init_with_config(
                   &feather, &(FeatherConfig){.allocator = &allocator.allocator,
                                              .now_fn = fake_clock_now,
                                              .now_context = &clock}),
               "allocator: Feather can boot with custom allocator");

  for (i = 0; i < 64; i++) {
    suite_expect(&suite,
                 Feather_add_deferred_task(
                     &feather,
                     (FSSchedulerDeferredTask){
                         .task = count_task,
                         .context = NULL,
                         .priority = FSScheduler_Priority_BACKGROUND,
                         .start_time = clock.now_ms + (uint64_t)i + 1}),
                 "allocator: scheduler accepts deferred task under custom allocator");
  }

  Feather_deinit(&feather);
  suite_expect(&suite, allocator.record_count == 0 && allocator.current_bytes == 0,
               "allocator: Feather deinit releases custom-allocator allocations");
  suite_expect(&suite, allocator.allocate_calls > 0 || allocator.reallocate_calls > 0,
               "allocator: allocator callbacks were exercised");
  suite_expect(&suite, allocator.peak_bytes > 0,
               "allocator: allocator observed a non-zero peak footprint");
  suite_expect(&suite,
               allocator.total_allocated_bytes == allocator.total_freed_bytes,
               "allocator: allocated and freed byte totals balance");

  suite_finish(&suite, 18.0);
}

static void run_scheduler_suite(Scoreboard *scoreboard,
                                uint64_t process_window_ms) {
  SuiteContext suite;
  struct Feather feather;
  FakeClock clock = {.now_ms = 1000};
  EventRecorder recorder;
  LogTaskContext interactive_log = {.recorder = &recorder, .token = 'I'};
  LogTaskContext ui_log = {.recorder = &recorder, .token = 'U'};
  LogTaskContext bg_log = {.recorder = &recorder, .token = 'B'};
  LogTaskContext delayed_bg = {.recorder = &recorder, .token = 'x'};
  LogTaskContext delayed_ui = {.recorder = &recorder, .token = 'u'};
  LogTaskContext delayed_interactive = {.recorder = &recorder, .token = 'i'};
  CounterTaskContext real_window_counter = {.counter = NULL};
  CounterTaskContext fixed_rate_counter = {.counter = NULL};
  AdvanceCounterTaskContext fixed_delay_counter = {.counter = NULL,
                                                   .clock = &clock,
                                                   .advance_ms = 25};
  int i;
  int counter = 0;
  int fixed_delay_runs = 0;
  int fixed_rate_runs = 0;
  uint64_t sleep_ms = 0;
  ProcessSnapshot window_before;
  ProcessSnapshot window_after;
  double window_elapsed_ms;
  double window_cpu_percent;

  suite_begin(&suite, scoreboard, "Scheduler Flow");

  event_recorder_reset(&recorder);
  suite_expect(&suite, Feather_init(&feather), "scheduler: init succeeds");
  for (i = 0; i < 6; i++) {
    suite_expect(&suite,
                 Feather_add_instant_task(
                     &feather,
                     (FSSchedulerInstantTask){.task = record_token_task,
                                              .context = &interactive_log,
                                              .priority = FSScheduler_Priority_INTERACTIVE}),
                 "scheduler: interactive task enqueued");
  }
  for (i = 0; i < 4; i++) {
    suite_expect(&suite,
                 Feather_add_instant_task(
                     &feather,
                     (FSSchedulerInstantTask){.task = record_token_task,
                                              .context = &ui_log,
                                              .priority = FSScheduler_Priority_UI}),
                 "scheduler: UI task enqueued");
  }
  for (i = 0; i < 3; i++) {
    suite_expect(&suite,
                 Feather_add_instant_task(
                     &feather,
                     (FSSchedulerInstantTask){.task = record_token_task,
                                              .context = &bg_log,
                                              .priority = FSScheduler_Priority_BACKGROUND}),
                 "scheduler: background task enqueued");
  }
  while (Feather_step(&feather)) {
  }
  suite_expect(&suite, strcmp(recorder.events, "IIIIUUBIIUUBB") == 0,
               "scheduler: ready-queue budgets rotate in priority order");
  Feather_deinit(&feather);

  event_recorder_reset(&recorder);
  clock.now_ms = 1000;
  suite_expect(&suite, Feather_init(&feather), "scheduler: delayed-order init succeeds");
  suite_expect(&suite, Feather_set_time_source(&feather, fake_clock_now, &clock),
               "scheduler: fake clock installed");
  suite_expect(&suite,
               Feather_add_instant_task(
                   &feather,
                   (FSSchedulerInstantTask){.task = record_token_task,
                                            .context = &delayed_bg,
                                            .priority = FSScheduler_Priority_BACKGROUND}),
               "scheduler: immediate delayed-order probe queued");
  suite_expect(&suite,
               Feather_add_deferred_task(
                   &feather,
                   (FSSchedulerDeferredTask){.task = record_token_task,
                                             .context = &delayed_ui,
                                             .priority = FSScheduler_Priority_UI,
                                             .start_time = 1100}),
               "scheduler: delayed UI probe queued");
  suite_expect(&suite,
               Feather_add_deferred_task(
                   &feather,
                   (FSSchedulerDeferredTask){.task = record_token_task,
                                             .context = &delayed_interactive,
                                             .priority = FSScheduler_Priority_INTERACTIVE,
                                             .start_time = 1100}),
               "scheduler: delayed interactive probe queued");
  suite_expect(&suite, Feather_step(&feather),
               "scheduler: immediate task executes before delayed peers");
  clock.now_ms = 1100;
  suite_expect(&suite, Feather_step(&feather) && Feather_step(&feather),
               "scheduler: delayed peers become ready together");
  suite_expect(&suite, strcmp(recorder.events, "xiu") == 0,
               "scheduler: delayed tie-break favours higher priority");
  Feather_deinit(&feather);

  clock.now_ms = 1000;
  fixed_rate_counter.counter = &fixed_rate_runs;
  suite_expect(&suite, Feather_init(&feather), "scheduler: fixed-rate init succeeds");
  suite_expect(&suite, Feather_set_time_source(&feather, fake_clock_now, &clock),
               "scheduler: fixed-rate fake clock installed");
  suite_expect(&suite,
               Feather_add_repeating_task(
                   &feather,
                   (FSSchedulerRepeatingTask){.task = count_task,
                                              .context = &fixed_rate_counter,
                                              .priority = FSScheduler_Priority_UI,
                                              .execute_cycle = 100,
                                              .repeat_mode = FSSchedulerTaskRepeat_FIXEDRATE,
                                              .start_time = clock.now_ms}),
               "scheduler: fixed-rate task queued");
  suite_expect(&suite, Feather_step(&feather), "scheduler: fixed-rate first execution runs");
  clock.now_ms = 1350;
  suite_expect(&suite, Feather_step(&feather), "scheduler: fixed-rate catches up once");
  clock.now_ms = 1399;
  suite_expect(&suite, !Feather_step(&feather),
               "scheduler: fixed-rate waits for next cadence boundary");
  clock.now_ms = 1400;
  suite_expect(&suite, Feather_step(&feather) && fixed_rate_runs == 3,
               "scheduler: fixed-rate resumes exactly on boundary");
  Feather_deinit(&feather);

  clock.now_ms = 1000;
  fixed_delay_counter.counter = &fixed_delay_runs;
  suite_expect(&suite, Feather_init(&feather), "scheduler: fixed-delay init succeeds");
  suite_expect(&suite, Feather_set_time_source(&feather, fake_clock_now, &clock),
               "scheduler: fixed-delay fake clock installed");
  suite_expect(&suite,
               Feather_add_repeating_task(
                   &feather,
                   (FSSchedulerRepeatingTask){.task = count_and_advance_task,
                                              .context = &fixed_delay_counter,
                                              .priority = FSScheduler_Priority_UI,
                                              .execute_cycle = 100,
                                              .repeat_mode = FSSchedulerTaskRepeat_FIXEDDELAY,
                                              .start_time = clock.now_ms}),
               "scheduler: fixed-delay task queued");
  suite_expect(&suite, Feather_step(&feather), "scheduler: fixed-delay first execution runs");
  clock.now_ms = 1124;
  suite_expect(&suite, !Feather_step(&feather),
               "scheduler: fixed-delay waits from completion time");
  clock.now_ms = 1125;
  suite_expect(&suite, Feather_step(&feather) && fixed_delay_runs == 2,
               "scheduler: fixed-delay fires on completion-based boundary");
  Feather_deinit(&feather);

  clock.now_ms = 2000;
  suite_expect(&suite, Feather_init(&feather), "scheduler: sleep-hint init succeeds");
  suite_expect(&suite, Feather_set_time_source(&feather, fake_clock_now, &clock),
               "scheduler: sleep-hint fake clock installed");
  suite_expect(&suite,
               Feather_add_deferred_task(
                   &feather,
                   (FSSchedulerDeferredTask){.task = count_task,
                                             .context = NULL,
                                             .priority = FSScheduler_Priority_BACKGROUND,
                                             .start_time = 2300}),
               "scheduler: delayed task queued for sleep-hint probe");
  suite_expect(&suite,
               Feather_next_sleep_ms(&feather, &sleep_ms) && sleep_ms == 300,
               "scheduler: next_sleep_ms reports remaining delay");
  clock.now_ms = 2300;
  suite_expect(&suite,
               Feather_next_sleep_ms(&feather, &sleep_ms) && sleep_ms == 0,
               "scheduler: next_sleep_ms returns zero once task is due");
  Feather_deinit(&feather);

  counter = 0;
  real_window_counter.counter = &counter;
  suite_expect(&suite, Feather_init(&feather), "scheduler: process-window init succeeds");
  suite_expect(&suite,
               Feather_add_repeating_task(
                   &feather,
                   (FSSchedulerRepeatingTask){.task = count_task,
                                              .context = &real_window_counter,
                                              .priority = FSScheduler_Priority_UI,
                                              .execute_cycle = 10,
                                              .repeat_mode = FSSchedulerTaskRepeat_FIXEDDELAY,
                                              .start_time = FSScheduler_now_ms()}),
               "scheduler: repeating task queued for process-window probe");
  capture_process_snapshot(&window_before);
  suite_expect(&suite, Feather_process_for_ms(&feather, process_window_ms),
               "scheduler: process_for_ms returns success");
  capture_process_snapshot(&window_after);
  window_elapsed_ms = (window_after.wall_seconds - window_before.wall_seconds) * 1000.0;
  window_cpu_percent = process_snapshot_cpu_percent(&window_before, &window_after);
  suite_expect(&suite, counter >= 3,
               "scheduler: process_for_ms executes repeating work during real window");
  suite_expect(&suite, window_elapsed_ms + 20.0 >= (double)process_window_ms,
               "scheduler: process_for_ms honours most of the requested window");
  Feather_deinit(&feather);

  suite_add_device_metric(&suite, "process window elapsed", clamp_score(
                                 25.0 * clamp_unit(1.0 - (absolute_double(window_elapsed_ms -
                                                                        (double)process_window_ms) /
                                                             ((double)process_window_ms * 0.75))),
                                 25.0),
                          25.0, window_elapsed_ms, " ms");
  suite_add_device_metric(&suite, "process window CPU", clamp_score(
                                 10.0 * clamp_unit(1.0 - (window_cpu_percent / 60.0)),
                                 10.0),
                          10.0, window_cpu_percent, "%");

  suite_finish(&suite, 30.0);
}

#if defined(FEATHER_SYSTEM_SCORE_HAS_RESOURCE_TRACKING)
static void run_tracking_suite(Scoreboard *scoreboard) {
  SuiteContext suite;
  struct Feather feather;
  FakeClock clock = {.now_ms = 4000};
  FSResourceTracker tracking;
  FSResourceTrackerSnapshot before_deinit;
  FSResourceTrackerSnapshot after_deinit;
  FSResourceTrackerRecord records[16];
  int i;

  suite_begin(&suite, scoreboard, "Resource Tracking");

  suite_expect(&suite, FSResourceTracker_init(&tracking),
               "tracking: tracker initializes");
  suite_expect(&suite, FSResourceTracker_allocator(&tracking) != NULL,
               "tracking: tracker exposes allocator wrapper");
  suite_expect(&suite,
               Feather_init_with_config(
                   &feather, &(FeatherConfig){.allocator = FSResourceTracker_allocator(&tracking),
                                              .now_fn = fake_clock_now,
                                              .now_context = &clock}),
               "tracking: Feather boots with tracking allocator");

  for (i = 0; i < 3; i++) {
    suite_expect(&suite,
                 Feather_add_instant_task(
                     &feather,
                     (FSSchedulerInstantTask){.task = count_task,
                                              .context = NULL,
                                              .priority = i}),
                 "tracking: ready queue task enqueued");
  }

  for (i = 0; i < 24; i++) {
    suite_expect(&suite,
                 Feather_add_deferred_task(
                     &feather,
                     (FSSchedulerDeferredTask){
                         .task = count_task,
                         .context = NULL,
                         .priority = FSScheduler_Priority_BACKGROUND,
                         .start_time = clock.now_ms + 50 + (uint64_t)i}),
                 "tracking: waiting task enqueued");
  }

  before_deinit = FSResourceTracker_snapshot(&tracking);
  suite_expect(&suite, before_deinit.active_allocations >= 2,
               "tracking: ready and waiting structures both allocate");
  suite_expect(&suite, before_deinit.current_bytes > 0,
               "tracking: tracker reports current bytes before deinit");
  suite_expect(&suite, FSResourceTracker_has_leaks(&tracking),
               "tracking: live allocations are visible before teardown");
  suite_expect(&suite,
               FSResourceTracker_copy_active_records(&tracking, records, 16) ==
                   before_deinit.active_allocations,
               "tracking: active records can be copied before teardown");

  Feather_deinit(&feather);
  after_deinit = FSResourceTracker_snapshot(&tracking);
  suite_expect(&suite, after_deinit.current_bytes == 0,
               "tracking: current bytes return to zero after Feather deinit");
  suite_expect(&suite, after_deinit.active_allocations == 0,
               "tracking: active allocations return to zero after Feather deinit");
  suite_expect(&suite,
               after_deinit.total_allocated_bytes ==
                   after_deinit.total_freed_bytes,
               "tracking: allocated and freed totals balance after teardown");
  suite_expect(&suite, after_deinit.peak_bytes >= before_deinit.current_bytes,
               "tracking: peak bytes preserve maximum lifetime usage");

  FSResourceTracker_deinit(&tracking);

  suite_finish(&suite, 15.0);
}
#else
static void run_tracking_suite(Scoreboard *scoreboard) {
  (void)scoreboard;
  printf("\n=== Resource Tracking ===\n");
  puts("  [skip] build was compiled without FeatherResourceTracking linkage");
}
#endif

static void run_endurance_suite(Scoreboard *scoreboard, uint64_t duration_ms) {
  SuiteContext suite;
  EnduranceHarness harness;
  bool workload_ok;
  double wall_before;
  double wall_after;
  double wall_delta;
  double throughput;
  double simulated_speed;
#if defined(FEATHER_SYSTEM_SCORE_HAS_RESOURCE_TRACKING)
  FSResourceTrackerSnapshot before_deinit;
  FSResourceTrackerSnapshot after_deinit;
#endif

  memset(&harness, 0, sizeof(harness));
  harness.clock.now_ms = 10000;
  suite_begin(&suite, scoreboard, "Extended Endurance");

#if defined(FEATHER_SYSTEM_SCORE_HAS_RESOURCE_TRACKING)
  suite_expect(&suite, FSResourceTracker_init(&harness.tracking),
               "endurance: tracking allocator initializes");
  suite_expect(
      &suite,
      Feather_init_with_config(
          &harness.feather,
          &(FeatherConfig){.allocator = FSResourceTracker_allocator(&harness.tracking),
                           .now_fn = fake_clock_now,
                           .now_context = &harness.clock}),
      "endurance: Feather boots with fake clock and tracking allocator");
#else
  suite_expect(
      &suite,
      Feather_init_with_config(&harness.feather,
                               &(FeatherConfig){.allocator = NULL,
                                                .now_fn = fake_clock_now,
                                                .now_context = &harness.clock}),
      "endurance: Feather boots with fake clock");
#endif

  suite_expect(&suite, endurance_seed(&harness),
               "endurance: recurring workload seeds successfully");

  wall_before = monotonic_seconds();
  workload_ok = run_endurance_workload(&harness, duration_ms);
  wall_after = monotonic_seconds();
  wall_delta = wall_after - wall_before;
  throughput = (wall_delta > 0.0)
                   ? ((double)harness.total_executions / wall_delta)
                   : 0.0;
  simulated_speed = (wall_delta > 0.0)
                        ? ((double)duration_ms / wall_delta)
                        : 0.0;

  suite_expect(&suite, workload_ok,
               "endurance: workload advances without runaway step loops");
  suite_expect(&suite, harness.enqueue_failures == 0,
               "endurance: nested task injections do not hit capacity failures");
  suite_expect(&suite,
               harness.interactive_runs > 0 && harness.ui_runs > 0 &&
                   harness.background_runs > 0,
               "endurance: all scheduler priorities execute meaningful work");
  suite_expect(&suite,
               harness.injector_runs > 0 && harness.child_runs > 0 &&
                   harness.delayed_probe_runs > 0,
               "endurance: injected child tasks and delayed probes execute");
  suite_expect(&suite, harness.pending_peak < 128,
               "endurance: mixed workload stays below a backlog explosion");
  suite_expect(&suite, harness.total_executions >= 1000,
               "endurance: extended run produces a large execution sample");

#if defined(FEATHER_SYSTEM_SCORE_HAS_RESOURCE_TRACKING)
  before_deinit = FSResourceTracker_snapshot(&harness.tracking);
  suite_expect(&suite, before_deinit.peak_bytes > 0,
               "endurance: tracking captures a non-zero internal peak footprint");
#endif

  Feather_deinit(&harness.feather);

#if defined(FEATHER_SYSTEM_SCORE_HAS_RESOURCE_TRACKING)
  after_deinit = FSResourceTracker_snapshot(&harness.tracking);
  suite_expect(&suite, after_deinit.current_bytes == 0,
               "endurance: tracking returns to zero bytes after teardown");
  suite_expect(&suite,
               after_deinit.total_allocated_bytes ==
                   after_deinit.total_freed_bytes,
               "endurance: tracked allocated and freed totals balance");
  FSResourceTracker_deinit(&harness.tracking);
#endif

  printf("  [metric] endurance steps=%" PRIu64 " ticks=%" PRIu64
         " executions=%d peak-pending=%d\n",
         harness.steps, harness.ticks, harness.total_executions,
         harness.pending_peak);
  printf("  [metric] endurance throughput=%.2f tasks/s simulated-speed=%.2f ms/s\n",
         throughput, simulated_speed);

  suite_add_device_metric(&suite, "simulated throughput",
                          clamp_score(45.0 * clamp_unit(throughput / 250000.0),
                                      45.0),
                          45.0, throughput, " tasks/s");
  suite_add_device_metric(&suite, "simulation speed",
                          clamp_score(20.0 * clamp_unit(simulated_speed / 500000.0),
                                      20.0),
                          20.0, simulated_speed, " sim-ms/s");
#if defined(FEATHER_SYSTEM_SCORE_HAS_RESOURCE_TRACKING)
  suite_add_device_metric(
      &suite, "tracked peak bytes",
      clamp_score(20.0 * clamp_unit(1.0 - ((double)before_deinit.peak_bytes / 262144.0)),
                  20.0),
      20.0, (double)before_deinit.peak_bytes, " B");
#else
  suite_add_device_metric(
      &suite, "pending depth headroom",
      clamp_score(20.0 * clamp_unit(1.0 - ((double)harness.pending_peak / 128.0)),
                  20.0),
      20.0, (double)harness.pending_peak, " tasks");
#endif
  suite_add_device_metric(
      &suite, "execution diversity",
      clamp_score(15.0 * clamp_unit((double)(harness.interactive_runs +
                                             harness.ui_runs +
                                             harness.background_runs) /
                                     5000.0),
                  15.0),
      15.0,
      (double)(harness.interactive_runs + harness.ui_runs +
               harness.background_runs),
      " runs");

  suite_finish(&suite, 25.0);
}

static const char *score_grade(double overall_score) {
  if (overall_score >= 97.0) {
    return "A+";
  }
  if (overall_score >= 93.0) {
    return "A";
  }
  if (overall_score >= 90.0) {
    return "A-";
  }
  if (overall_score >= 87.0) {
    return "B+";
  }
  if (overall_score >= 83.0) {
    return "B";
  }
  if (overall_score >= 80.0) {
    return "B-";
  }
  if (overall_score >= 77.0) {
    return "C+";
  }
  if (overall_score >= 73.0) {
    return "C";
  }
  if (overall_score >= 70.0) {
    return "C-";
  }
  if (overall_score >= 60.0) {
    return "D";
  }
  return "F";
}

int main(int argc, char **argv) {
  RuntimeConfig config;
  Scoreboard scoreboard;
  double flow_score;
  double device_score;
  double overall_score;

  if (!parse_args(argc, argv, &config)) {
    print_usage(argv[0]);
    return EXIT_FAILURE;
  }

  memset(&scoreboard, 0, sizeof(scoreboard));

  printf("Feather Extended System Score Test\n");
  printf("==================================\n");
  printf("duration-ms=%" PRIu64 " process-window-ms=%" PRIu64 "\n",
         config.duration_ms, config.process_window_ms);

  run_api_surface_suite(&scoreboard);
  run_allocator_suite(&scoreboard);
  run_scheduler_suite(&scoreboard, config.process_window_ms);
  run_tracking_suite(&scoreboard);
  run_endurance_suite(&scoreboard, config.duration_ms);

  flow_score = (scoreboard.flow_possible > 0.0)
                   ? (100.0 * scoreboard.flow_earned / scoreboard.flow_possible)
                   : 100.0;
  device_score = (scoreboard.device_possible > 0.0)
                     ? (100.0 * scoreboard.device_earned /
                        scoreboard.device_possible)
                     : flow_score;
  overall_score = (flow_score * 0.65) + (device_score * 0.35);

  printf("\n=== Final Score ===\n");
  printf("Assertions : %d\n", scoreboard.assertions);
  printf("Failures   : %d\n", scoreboard.failures);
  printf("Suites     : %d total / %d failed\n", scoreboard.suites_run,
         scoreboard.suites_failed);
  printf("Flow Score : %.2f / 100\n", flow_score);
  printf("Device Score: %.2f / 100\n", device_score);
  printf("Overall    : %.2f / 100 (%s)\n", overall_score,
         score_grade(overall_score));

  if (scoreboard.failures == 0) {
    puts("Result     : PASS");
    return EXIT_SUCCESS;
  }

  puts("Result     : FAIL");
  return EXIT_FAILURE;
}
