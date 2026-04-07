#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#define _POSIX_C_SOURCE 200809L

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/task_info.h>
#endif

#include "Feather.hpp"
#include "FeatherRuntime/FSResourceTracker.hpp"

#define RUN_SECONDS 30.0
#define SNAPSHOT_INTERVAL_SECONDS 1.0

#define PROCESS_WINDOW_MS 25ULL
#define MANUAL_STEP_BURST 8

#define INTERACTIVE_PERIOD_MS 20ULL
#define UI_PERIOD_MS 40ULL
#define BG_PERIOD_MS 80ULL

#define INTERACTIVE_WORK_ITERS 45000UL
#define UI_WORK_ITERS 90000UL
#define BG_WORK_ITERS 180000UL
#define BURST_CHILD_WORK_ITERS 35000UL
#define DELAYED_PROBE_WORK_ITERS 70000UL

#define INTERACTIVE_SCRATCH_KB 32U
#define UI_SCRATCH_KB 64U
#define BG_SCRATCH_KB 96U
#define DELAYED_SCRATCH_KB 128U

#define BURST_TASK_COUNT 64
#define INJECT_EVERY_N_INTERACTIVE 8ULL

typedef struct ProcessSnapshot {
  double wall_s;
  double cpu_s;
  long rss_kb;
  long hwm_kb;
  long threads;
  long voluntary_ctxt;
  long involuntary_ctxt;
} ProcessSnapshot;

typedef struct BenchStats {
  uint64_t interactive_runs;
  uint64_t ui_runs;
  uint64_t bg_runs;
  uint64_t burst_injector_runs;
  uint64_t burst_child_runs;
  uint64_t delayed_probe_runs;

  uint64_t step_calls;
  uint64_t step_successes;
  uint64_t process_windows;
  uint64_t sleep_hint_calls;
  uint64_t sleep_hint_nonzero;
  uint64_t max_sleep_hint_ms;

  uint64_t snapshots;
  long peak_rss_kb;
  long peak_hwm_kb;
  long peak_threads;

  uint64_t tracked_peak_bytes;
  uint64_t tracked_peak_active_allocations;
  uint64_t tracked_total_allocated_peak;

  uint64_t checksum;
  bool saw_pending_true;
  bool saw_pending_false;
} BenchStats;

struct Harness;

typedef struct TaskCtx {
  struct Harness *h;
  uint32_t seed;
  uint32_t scratch_kb;
} TaskCtx;

typedef struct Harness {
  Feather feather;
  FSResourceTracker tracking;
  BenchStats stats;
  uint64_t start_ms;
  uint64_t deadline_ms;

  TaskCtx interactive_ctx;
  TaskCtx ui_ctx;
  TaskCtx bg_ctx;
  TaskCtx delayed_probe_ctx;
} Harness;

static double monotonic_seconds(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
    return 0.0;
  }
  return (double)ts.tv_sec + ((double)ts.tv_nsec / 1000000000.0);
}

static double process_cpu_seconds(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) != 0) {
    return 0.0;
  }
  return (double)ts.tv_sec + ((double)ts.tv_nsec / 1000000000.0);
}

static uint64_t monotonic_ms_now(void *context) {
  (void)context;
  return (uint64_t)(monotonic_seconds() * 1000.0);
}

static long read_proc_status_kb(const char *key) {
#if defined(__APPLE__)
  (void)key;
  return -1;
#else
  FILE *fp;
  char line[256];
  size_t key_len;
  long value = -1;

  if (key == NULL) {
    return -1;
  }

  fp = fopen("/proc/self/status", "r");
  if (fp == NULL) {
    return -1;
  }

  key_len = strlen(key);
  while (fgets(line, sizeof(line), fp) != NULL) {
    if (strncmp(line, key, key_len) == 0) {
      value = strtol(line + (long)key_len, NULL, 10);
      break;
    }
  }

  fclose(fp);
  return value;
#endif
}

static void capture_snapshot(ProcessSnapshot *out) {
  struct rusage usage;

  if (out == NULL) {
    return;
  }

  memset(out, 0, sizeof(*out));
  out->wall_s = monotonic_seconds();
  out->cpu_s = process_cpu_seconds();
  out->rss_kb = -1;
  out->hwm_kb = -1;
  out->threads = -1;
  out->voluntary_ctxt = -1;
  out->involuntary_ctxt = -1;

#if defined(__APPLE__)
  {
    mach_task_basic_info_data_t info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;

    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info,
                  &count) == KERN_SUCCESS) {
      out->rss_kb = (long)(info.resident_size / 1024ULL);
      out->hwm_kb = (long)(info.resident_size_max / 1024ULL);
    }
  }
#else
  out->rss_kb = read_proc_status_kb("VmRSS:");
  out->hwm_kb = read_proc_status_kb("VmHWM:");
  out->threads = read_proc_status_kb("Threads:");
#endif

  if (getrusage(RUSAGE_SELF, &usage) == 0) {
    out->voluntary_ctxt = usage.ru_nvcsw;
    out->involuntary_ctxt = usage.ru_nivcsw;
  }
}

static uint64_t mix64(uint64_t x) {
  x ^= x >> 33;
  x *= UINT64_C(0xff51afd7ed558ccd);
  x ^= x >> 33;
  x *= UINT64_C(0xc4ceb9fe1a85ec53);
  x ^= x >> 33;
  return x;
}

static uint64_t do_busy_work(uint32_t *seed, unsigned long iters,
                             uint32_t scratch_kb) {
  size_t bytes = (size_t)scratch_kb * 1024U;
  uint8_t *scratch = NULL;
  uint64_t acc;
  unsigned long i;

  if (seed == NULL) {
    return 0;
  }

  acc = (uint64_t)(*seed) + UINT64_C(0x9e3779b97f4a7c15);

  if (bytes > 0) {
    scratch = (uint8_t *)malloc(bytes);
    if (scratch != NULL) {
      memset(scratch, 0, bytes);
    }
  }

  for (i = 0; i < iters; i++) {
    acc = mix64(acc + (uint64_t)i + UINT64_C(0x9e3779b185ebca87));
    acc ^= (acc << 7);
    acc ^= (acc >> 11);

    if (scratch != NULL) {
      size_t idx = (size_t)(acc % bytes);
      scratch[idx] = (uint8_t)(scratch[idx] + (uint8_t)(acc & 0xFFu));
    }
  }

  if (scratch != NULL) {
    size_t j;
    for (j = 0; j < bytes; j += 4096U) {
      acc ^= scratch[j];
    }
    free(scratch);
  }

  *seed = (uint32_t)(acc & 0xffffffffu);
  return acc;
}

static void burst_child_task(void *context) {
  TaskCtx *child = (TaskCtx *)context;
  Harness *h;

  if (child == NULL || child->h == NULL) {
    free(child);
    return;
  }

  h = child->h;
  h->stats.burst_child_runs++;
  h->stats.checksum ^= do_busy_work(&child->seed, BURST_CHILD_WORK_ITERS,
                                    child->scratch_kb);

  free(child);
}

static void schedule_burst_children(Harness *h, uint64_t now_ms) {
  int i;

  if (h == NULL) {
    return;
  }

  for (i = 0; i < BURST_TASK_COUNT; i++) {
    TaskCtx *ctx = (TaskCtx *)malloc(sizeof(TaskCtx));
    FSSchedulerDeferredTask task = FSSchedulerDeferredTask_init;

    if (ctx == NULL) {
      continue;
    }

    ctx->h = h;
    ctx->seed = (uint32_t)(0x12340000u + (uint32_t)(i * 97) +
                           (uint32_t)h->stats.burst_injector_runs);
    ctx->scratch_kb = (uint32_t)(8 + ((unsigned)i % 8U) * 4U);

    task.task = burst_child_task;
    task.priority = (i % 3 == 0) ? FSScheduler_Priority_INTERACTIVE
                  : (i % 3 == 1) ? FSScheduler_Priority_UI
                                 : FSScheduler_Priority_BACKGROUND;
    task.start_time = now_ms + (uint64_t)(5 + (i % 16) * 5);
    task.context = ctx;

    if (!Feather_add_deferred_task(&h->feather, task)) {
      free(ctx);
    }
  }
}

static void interactive_task(void *context) {
  TaskCtx *ctx = (TaskCtx *)context;
  Harness *h;

  if (ctx == NULL || ctx->h == NULL) {
    return;
  }

  h = ctx->h;
  h->stats.interactive_runs++;
  h->stats.checksum ^= do_busy_work(&ctx->seed, INTERACTIVE_WORK_ITERS,
                                    ctx->scratch_kb);

  if ((h->stats.interactive_runs % INJECT_EVERY_N_INTERACTIVE) == 0U) {
    h->stats.burst_injector_runs++;
    schedule_burst_children(h, monotonic_ms_now(NULL));
  }
}

static void ui_task(void *context) {
  TaskCtx *ctx = (TaskCtx *)context;
  Harness *h;

  if (ctx == NULL || ctx->h == NULL) {
    return;
  }

  h = ctx->h;
  h->stats.ui_runs++;
  h->stats.checksum ^= do_busy_work(&ctx->seed, UI_WORK_ITERS,
                                    ctx->scratch_kb);
}

static void bg_task(void *context) {
  TaskCtx *ctx = (TaskCtx *)context;
  Harness *h;

  if (ctx == NULL || ctx->h == NULL) {
    return;
  }

  h = ctx->h;
  h->stats.bg_runs++;
  h->stats.checksum ^= do_busy_work(&ctx->seed, BG_WORK_ITERS,
                                    ctx->scratch_kb);
}

static void delayed_probe_task(void *context) {
  TaskCtx *ctx = (TaskCtx *)context;
  Harness *h;

  if (ctx == NULL || ctx->h == NULL) {
    return;
  }

  h = ctx->h;
  h->stats.delayed_probe_runs++;
  h->stats.checksum ^= do_busy_work(&ctx->seed, DELAYED_PROBE_WORK_ITERS,
                                    ctx->scratch_kb);
}

static void print_runtime_line(const Harness *h, const ProcessSnapshot *snap,
                               const FSResourceTrackerSnapshot *trs) {
  double elapsed_s;

  if (h == NULL || snap == NULL || trs == NULL) {
    return;
  }

  elapsed_s = snap->wall_s - ((double)h->start_ms / 1000.0);

  printf("[t=%6.2fs] cpu=%6.2fs rss=%8ld KB hwm=%8ld KB thr=%ld ",
         elapsed_s, snap->cpu_s, snap->rss_kb, snap->hwm_kb, snap->threads);
  printf("I=%" PRIu64 " U=%" PRIu64 " B=%" PRIu64 " ",
         h->stats.interactive_runs, h->stats.ui_runs, h->stats.bg_runs);
  printf("burst=%" PRIu64 " child=%" PRIu64 " delayed=%" PRIu64 " ",
         h->stats.burst_injector_runs, h->stats.burst_child_runs,
         h->stats.delayed_probe_runs);
  printf("tracked(cur=%zu peak=%zu active=%zu)\n", trs->current_bytes,
         trs->peak_bytes, trs->active_allocations);
}

int main(void) {
  Harness h;
  FeatherConfig feather_config = FeatherConfig_init;
  FSResourceTrackerConfig tracking_config = FSResourceTrackerConfig_init;
  FSResourceTrackerSnapshot tracking_before_deinit;
  FSResourceTrackerSnapshot tracking_after_deinit;
  double next_snapshot_at;
  bool ok = true;

  memset(&h, 0, sizeof(h));

  tracking_config.now_fn = monotonic_ms_now;
  tracking_config.now_context = NULL;

  if (!FSResourceTracker_init_with_config(&h.tracking, &tracking_config)) {
    fprintf(stderr, "FSResourceTracker_init_with_config failed\n");
    return 1;
  }

  feather_config.allocator = FSResourceTracker_allocator(&h.tracking);
  feather_config.now_fn = monotonic_ms_now;
  feather_config.now_context = NULL;

  if (!Feather_init_with_config(&h.feather, &feather_config)) {
    fprintf(stderr, "Feather_init_with_config failed\n");
    FSResourceTracker_deinit(&h.tracking);
    return 1;
  }

  if (!Feather_set_time_source(&h.feather, monotonic_ms_now, NULL)) {
    fprintf(stderr, "Feather_set_time_source failed\n");
    Feather_deinit(&h.feather);
    FSResourceTracker_deinit(&h.tracking);
    return 1;
  }

  h.start_ms = monotonic_ms_now(NULL);
  h.deadline_ms = h.start_ms + (uint64_t)(RUN_SECONDS * 1000.0);

  h.interactive_ctx.h = &h;
  h.interactive_ctx.seed = 0x11111111u;
  h.interactive_ctx.scratch_kb = INTERACTIVE_SCRATCH_KB;

  h.ui_ctx.h = &h;
  h.ui_ctx.seed = 0x22222222u;
  h.ui_ctx.scratch_kb = UI_SCRATCH_KB;

  h.bg_ctx.h = &h;
  h.bg_ctx.seed = 0x33333333u;
  h.bg_ctx.scratch_kb = BG_SCRATCH_KB;

  h.delayed_probe_ctx.h = &h;
  h.delayed_probe_ctx.seed = 0x44444444u;
  h.delayed_probe_ctx.scratch_kb = DELAYED_SCRATCH_KB;

  {
    FSSchedulerRepeatingTask interactive = FSSchedulerRepeatingTask_init;
    FSSchedulerRepeatingTask ui = FSSchedulerRepeatingTask_init;
    FSSchedulerRepeatingTask bg = FSSchedulerRepeatingTask_init;
    FSSchedulerDeferredTask delayed_probe = FSSchedulerDeferredTask_init;

    interactive.task = interactive_task;
    interactive.context = &h.interactive_ctx;
    interactive.priority = FSScheduler_Priority_INTERACTIVE;
    interactive.start_time = h.start_ms;
    interactive.execute_cycle = INTERACTIVE_PERIOD_MS;
    interactive.repeat_mode = FSSchedulerTaskRepeat_FIXEDRATE;

    ui.task = ui_task;
    ui.context = &h.ui_ctx;
    ui.priority = FSScheduler_Priority_UI;
    ui.start_time = h.start_ms;
    ui.execute_cycle = UI_PERIOD_MS;
    ui.repeat_mode = FSSchedulerTaskRepeat_FIXEDDELAY;

    bg.task = bg_task;
    bg.context = &h.bg_ctx;
    bg.priority = FSScheduler_Priority_BACKGROUND;
    bg.start_time = h.start_ms;
    bg.execute_cycle = BG_PERIOD_MS;
    bg.repeat_mode = FSSchedulerTaskRepeat_FIXEDDELAY;

    delayed_probe.task = delayed_probe_task;
    delayed_probe.context = &h.delayed_probe_ctx;
    delayed_probe.priority = FSScheduler_Priority_UI;
    delayed_probe.start_time = h.start_ms + 3000ULL;

    if (!Feather_add_repeating_task(&h.feather, interactive) ||
        !Feather_add_repeating_task(&h.feather, ui) ||
        !Feather_add_repeating_task(&h.feather, bg) ||
        !Feather_add_deferred_task(&h.feather, delayed_probe)) {
      fprintf(stderr, "initial task scheduling failed\n");
      Feather_deinit(&h.feather);
      FSResourceTracker_deinit(&h.tracking);
      return 1;
    }
  }

  printf("Feather full-system benchmark start\n");
  printf("target runtime: %.0f sec\n", RUN_SECONDS);

  next_snapshot_at = monotonic_seconds();

  while (monotonic_ms_now(NULL) < h.deadline_ms) {
    uint64_t sleep_hint_ms = 0;
    FSResourceTrackerSnapshot trs;
    ProcessSnapshot snap;
    int i;

    h.stats.process_windows++;
    (void)Feather_process_for_ms(&h.feather, PROCESS_WINDOW_MS);

    for (i = 0; i < MANUAL_STEP_BURST; i++) {
      h.stats.step_calls++;
      if (Feather_step(&h.feather)) {
        h.stats.step_successes++;
      } else {
        break;
      }
    }

    if (Feather_has_pending_tasks(&h.feather)) {
      h.stats.saw_pending_true = true;
    } else {
      h.stats.saw_pending_false = true;
    }

    if (Feather_next_sleep_ms(&h.feather, &sleep_hint_ms)) {
      h.stats.sleep_hint_calls++;
      if (sleep_hint_ms > 0) {
        h.stats.sleep_hint_nonzero++;
      }
      if (sleep_hint_ms > h.stats.max_sleep_hint_ms) {
        h.stats.max_sleep_hint_ms = sleep_hint_ms;
      }
    }

    trs = FSResourceTracker_snapshot(&h.tracking);
    if ((uint64_t)trs.peak_bytes > h.stats.tracked_peak_bytes) {
      h.stats.tracked_peak_bytes = (uint64_t)trs.peak_bytes;
    }
    if ((uint64_t)trs.active_allocations >
        h.stats.tracked_peak_active_allocations) {
      h.stats.tracked_peak_active_allocations =
          (uint64_t)trs.active_allocations;
    }
    if ((uint64_t)trs.total_allocated_bytes >
        h.stats.tracked_total_allocated_peak) {
      h.stats.tracked_total_allocated_peak =
          (uint64_t)trs.total_allocated_bytes;
    }

    if (monotonic_seconds() >= next_snapshot_at) {
      capture_snapshot(&snap);
      h.stats.snapshots++;

      if (snap.rss_kb > h.stats.peak_rss_kb) {
        h.stats.peak_rss_kb = snap.rss_kb;
      }
      if (snap.hwm_kb > h.stats.peak_hwm_kb) {
        h.stats.peak_hwm_kb = snap.hwm_kb;
      }
      if (snap.threads > h.stats.peak_threads) {
        h.stats.peak_threads = snap.threads;
      }

      print_runtime_line(&h, &snap, &trs);
      next_snapshot_at += SNAPSHOT_INTERVAL_SECONDS;
    }
  }

  tracking_before_deinit = FSResourceTracker_snapshot(&h.tracking);

  while (Feather_step(&h.feather)) {
    h.stats.step_calls++;
    h.stats.step_successes++;
  }

  Feather_deinit(&h.feather);
  tracking_after_deinit = FSResourceTracker_snapshot(&h.tracking);

  printf("\n=== summary ===\n");
  printf("wall target              : %.0f sec\n", RUN_SECONDS);
  printf("interactive runs         : %" PRIu64 "\n", h.stats.interactive_runs);
  printf("ui runs                  : %" PRIu64 "\n", h.stats.ui_runs);
  printf("background runs          : %" PRIu64 "\n", h.stats.bg_runs);
  printf("burst injector runs      : %" PRIu64 "\n",
         h.stats.burst_injector_runs);
  printf("burst child runs         : %" PRIu64 "\n", h.stats.burst_child_runs);
  printf("delayed probe runs       : %" PRIu64 "\n",
         h.stats.delayed_probe_runs);
  printf("step calls / successes   : %" PRIu64 " / %" PRIu64 "\n",
         h.stats.step_calls, h.stats.step_successes);
  printf("process windows          : %" PRIu64 "\n", h.stats.process_windows);
  printf("sleep hints(nonzero/max) : %" PRIu64 " / %" PRIu64 " / %" PRIu64
         " ms\n",
         h.stats.sleep_hint_calls, h.stats.sleep_hint_nonzero,
         h.stats.max_sleep_hint_ms);
  printf("peak rss / hwm           : %ld KB / %ld KB\n", h.stats.peak_rss_kb,
         h.stats.peak_hwm_kb);
  printf("peak threads             : %ld\n", h.stats.peak_threads);
  printf("tracked peak bytes       : %" PRIu64 "\n", h.stats.tracked_peak_bytes);
  printf("tracked peak active      : %" PRIu64 "\n",
         h.stats.tracked_peak_active_allocations);
  printf("tracked total allocated  : %" PRIu64 "\n",
         h.stats.tracked_total_allocated_peak);
  printf("tracked before deinit    : current=%zu peak=%zu active=%zu total_alloc=%zu "
         "total_free=%zu\n",
         tracking_before_deinit.current_bytes, tracking_before_deinit.peak_bytes,
         tracking_before_deinit.active_allocations,
         tracking_before_deinit.total_allocated_bytes,
         tracking_before_deinit.total_freed_bytes);
  printf("tracked after deinit     : current=%zu peak=%zu active=%zu total_alloc=%zu "
         "total_free=%zu\n",
         tracking_after_deinit.current_bytes, tracking_after_deinit.peak_bytes,
         tracking_after_deinit.active_allocations,
         tracking_after_deinit.total_allocated_bytes,
         tracking_after_deinit.total_freed_bytes);
  printf("checksum                 : %" PRIu64 "\n", h.stats.checksum);

  if (h.stats.interactive_runs == 0) {
    ok = false;
  }
  if (h.stats.ui_runs == 0) {
    ok = false;
  }
  if (h.stats.bg_runs == 0) {
    ok = false;
  }
  if (h.stats.burst_child_runs == 0) {
    ok = false;
  }
  if (h.stats.delayed_probe_runs == 0) {
    ok = false;
  }
  if (!h.stats.saw_pending_true) {
    ok = false;
  }
  if (tracking_after_deinit.current_bytes != 0) {
    ok = false;
  }
  if (tracking_after_deinit.active_allocations != 0) {
    ok = false;
  }
  if (tracking_after_deinit.total_allocated_bytes !=
      tracking_after_deinit.total_freed_bytes) {
    ok = false;
  }

  FSResourceTracker_deinit(&h.tracking);

  printf("\nRESULT: %s\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
