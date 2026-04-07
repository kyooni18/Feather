#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <libproc.h>
#include <mach/mach.h>
#include <mach/task_info.h>
#endif

#include "Feather.h"

#define CHECKSUM_MOD 1000000007LL
#define CHECKSUM_MOD2 998244353LL
#define LIVE_REFRESH_INTERVAL_SEC 0.20
#define SNAPSHOT_INTERVAL_SEC 1.00
#define IDLE_SLEEP_NSEC 50000000L
#define SAFETY_MAX_STEPS 4096
#define TAIL_CPU_SECONDS 5.0

static struct Feather *active_feather = NULL;
static bool demo_failed = false;

static FILE *g_log_file = NULL;
static FILE *g_metrics_file = NULL;
static char g_log_path[256] = {0};
static char g_metrics_path[256] = {0};
static int g_live_line_visible = 0;

static struct {
  double wall_start;
  double cpu_start;
  double last_live_refresh;
  double last_snapshot;
  double last_cpu_sample;
  double last_wall_sample;
  double last_cpu_percent;
  long long scheduler_steps;
  unsigned long long tail_iterations;
  bool stop_requested;
} runtime_state;

typedef struct ComplexAnalytics {
  int start_value;
  int end_value;
  int next_value;
  int chunk_size;
  int chunks_done;
  int total_chunks;

  long long count;
  long long sum;
  long long sum_squares;
  long long sum_cubes;
  long long sum_fourths;
  long long sum_fifths;
  long long weighted_polynomial;
  long long alternating_cubic_balance;
  long long divisor_total;
  long long prime_count;
  long long prime_cube_total;
  long long residue_signature;
  long long euler_totient_total;
  long long collatz_total;
  long long checksum;
  long long checksum2;

  long long expected_sum;
  long long expected_sum_squares;
  long long expected_sum_cubes;
  long long expected_sum_fourths;
  long long expected_sum_fifths;

  bool sum_valid;
  bool sum_squares_valid;
  bool sum_cubes_valid;
  bool sum_fourths_valid;
  bool sum_fifths_valid;

  long double mean;
  long double raw_second_moment;
  long double raw_third_moment;
  long double raw_fourth_moment;
  long double raw_fifth_moment;
  long double variance;
  long double third_central_moment;
  long double fourth_central_moment;
  long double prime_density;
  long double normalized_energy;
  long double stability_index;
} ComplexAnalytics;

static ComplexAnalytics analytics;

typedef struct ProcessSnapshot {
  double wall_seconds;
  double proc_cpu_seconds;
  double cpu_percent;
  long rss_kb;
  long hwm_kb;
  long vm_size_kb;
  long threads;
  long voluntary_ctxt;
  long involuntary_ctxt;
  int open_fds;
  pid_t pid;
  pid_t ppid;
} ProcessSnapshot;

static double monotonic_seconds(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

static double process_cpu_seconds(void) {
  struct timespec ts;
  clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

static void clear_live_line_if_needed(void) {
  if (g_live_line_visible) {
    fputc('\r', stdout);
    fputs("                                                                    "
          "                                                    \r",
          stdout);
    fflush(stdout);
    g_live_line_visible = 0;
  }
}

static int demo_vprintf(const char *fmt, va_list ap) {
  if (g_log_file != NULL) {
    vfprintf(g_log_file, fmt, ap);
    fflush(g_log_file);
  }
  return 0;
}

static int demo_printf_impl(const char *fmt, ...) {
  va_list ap;
  int rc;

  va_start(ap, fmt);
  rc = demo_vprintf(fmt, ap);
  va_end(ap);
  return rc;
}

static int demo_puts_impl(const char *s) {
  if (g_log_file != NULL) {
    if (fputs(s, g_log_file) == EOF || fputc('\n', g_log_file) == EOF) {
      return EOF;
    }
    fflush(g_log_file);
  }

  return 0;
}

#define printf demo_printf_impl
#define puts demo_puts_impl

static int status_puts(const char *s) {
  clear_live_line_if_needed();
  if (fputs(s, stdout) == EOF || fputc('\n', stdout) == EOF) {
    return EOF;
  }
  fflush(stdout);
  if (g_log_file != NULL) {
    if (fputs(s, g_log_file) == EOF || fputc('\n', g_log_file) == EOF) {
      return EOF;
    }
    fflush(g_log_file);
  }
  return 0;
}

static int status_printf(const char *fmt, ...) {
  va_list ap, ap_copy;
  int rc;

  clear_live_line_if_needed();
  va_start(ap, fmt);
  va_copy(ap_copy, ap);
  rc = vfprintf(stdout, fmt, ap);
  fflush(stdout);
  if (g_log_file != NULL) {
    if (vfprintf(g_log_file, fmt, ap_copy) < 0) {
      rc = EOF;
    }
    fflush(g_log_file);
  }
  va_end(ap_copy);
  va_end(ap);
  return rc;
}

static void format_time_string(char *buf, size_t buf_size, time_t now) {
  struct tm tm_now;
  localtime_r(&now, &tm_now);
  strftime(buf, buf_size, "%Y%m%d_%H%M%S", &tm_now);
}

static void initialize_logs(void) {
  char stamp[64];
  time_t now = time(NULL);

  mkdir("logs", 0755);
  format_time_string(stamp, sizeof(stamp), now);

  snprintf(g_log_path, sizeof(g_log_path), "logs/demo_run_%s.log", stamp);
  snprintf(g_metrics_path, sizeof(g_metrics_path), "logs/demo_metrics_%s.csv",
           stamp);

  g_log_file = fopen(g_log_path, "w");
  g_metrics_file = fopen(g_metrics_path, "w");

  if (g_metrics_file != NULL) {
    fprintf(g_metrics_file,
            "wall_s,proc_cpu_s,cpu_pct,rss_kb,hwm_kb,vm_kb,threads,open_fds,"
            "voluntary_ctxt,involuntary_ctxt,steps,chunks_done,next_value\n");
    fflush(g_metrics_file);
  }
}

static long read_status_long_value_generic(const char *key) {
#if defined(__APPLE__)
  (void)key;
  return -1;
#else
  FILE *fp;
  char line[256];
  long value = -1;

  fp = fopen("/proc/self/status", "r");
  if (fp == NULL) {
    return -1;
  }

  while (fgets(line, sizeof(line), fp) != NULL) {
    char *colon;
    char *endptr;

    if (strncmp(line, key, strlen(key)) != 0) {
      continue;
    }

    colon = strchr(line, ':');
    if (colon == NULL) {
      continue;
    }

    value = strtol(colon + 1, &endptr, 10);
    if (endptr != colon + 1) {
      fclose(fp);
      return value;
    }
  }

  fclose(fp);
  return -1;
#endif
}

static int count_open_fds(void) {
#if defined(__APPLE__)
  long fd;
  long max_fd = sysconf(_SC_OPEN_MAX);
  int count = 0;

  if (max_fd <= 0) {
    return -1;
  }

  for (fd = 0; fd < max_fd; fd++) {
    errno = 0;
    if (fcntl((int)fd, F_GETFD) != -1 || errno != EBADF) {
      count++;
    }
  }

  return count;
#else
  DIR *dir;
  struct dirent *entry;
  int count = 0;

  dir = opendir("/proc/self/fd");
  if (dir == NULL) {
    return -1;
  }

  while ((entry = readdir(dir)) != NULL) {
    if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
      count++;
    }
  }

  closedir(dir);
  return count;
#endif
}

static long bytes_to_kb(uint64_t bytes) { return (long)(bytes / 1024ULL); }

static void collect_platform_process_metrics(ProcessSnapshot *snapshot) {
#if defined(__APPLE__)
  struct proc_taskinfo taskinfo;
  task_vm_info_data_t vm_info;
  mach_msg_type_number_t vm_info_count = TASK_VM_INFO_COUNT;
  int taskinfo_size;

  snapshot->rss_kb = -1;
  snapshot->hwm_kb = -1;
  snapshot->vm_size_kb = -1;
  snapshot->threads = -1;

  taskinfo_size = proc_pidinfo(getpid(), PROC_PIDTASKINFO, 0, &taskinfo,
                               PROC_PIDTASKINFO_SIZE);
  if (taskinfo_size == PROC_PIDTASKINFO_SIZE) {
    snapshot->rss_kb = bytes_to_kb(taskinfo.pti_resident_size);
    snapshot->vm_size_kb = bytes_to_kb(taskinfo.pti_virtual_size);
    snapshot->threads = taskinfo.pti_threadnum;
  }

  if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vm_info,
                &vm_info_count) == KERN_SUCCESS) {
    snapshot->hwm_kb = bytes_to_kb(vm_info.resident_size_peak);
  }
#else
  snapshot->rss_kb = read_status_long_value_generic("VmRSS");
  snapshot->hwm_kb = read_status_long_value_generic("VmHWM");
  snapshot->vm_size_kb = read_status_long_value_generic("VmSize");
  snapshot->threads = read_status_long_value_generic("Threads");
#endif
}

static void collect_process_snapshot(ProcessSnapshot *snapshot) {
  struct rusage usage;
  double now_wall = monotonic_seconds();
  double now_cpu = process_cpu_seconds();
  double wall_delta = now_wall - runtime_state.last_wall_sample;
  double cpu_delta = now_cpu - runtime_state.last_cpu_sample;

  memset(snapshot, 0, sizeof(*snapshot));
  snapshot->wall_seconds = now_wall - runtime_state.wall_start;
  snapshot->proc_cpu_seconds = now_cpu - runtime_state.cpu_start;

  if (wall_delta >= 0.05) {
    runtime_state.last_cpu_percent = (cpu_delta / wall_delta) * 100.0;
  } else if (runtime_state.scheduler_steps == 0 && analytics.chunks_done == 0 &&
             runtime_state.tail_iterations == 0) {
    runtime_state.last_cpu_percent = 0.0;
  }
  snapshot->cpu_percent = runtime_state.last_cpu_percent;

  collect_platform_process_metrics(snapshot);
  snapshot->open_fds = count_open_fds();
  snapshot->pid = getpid();
  snapshot->ppid = getppid();

  if (getrusage(RUSAGE_SELF, &usage) == 0) {
    snapshot->voluntary_ctxt = usage.ru_nvcsw;
    snapshot->involuntary_ctxt = usage.ru_nivcsw;
  } else {
    snapshot->voluntary_ctxt = -1;
    snapshot->involuntary_ctxt = -1;
  }

  runtime_state.last_wall_sample = now_wall;
  runtime_state.last_cpu_sample = now_cpu;
}

static void write_metrics_row(const ProcessSnapshot *snapshot) {
  if (g_metrics_file == NULL) {
    return;
  }

  fprintf(
      g_metrics_file, "%.3f,%.3f,%.2f,%ld,%ld,%ld,%ld,%d,%ld,%ld,%lld,%d,%d\n",
      snapshot->wall_seconds, snapshot->proc_cpu_seconds, snapshot->cpu_percent,
      snapshot->rss_kb, snapshot->hwm_kb, snapshot->vm_size_kb,
      snapshot->threads, snapshot->open_fds, snapshot->voluntary_ctxt,
      snapshot->involuntary_ctxt, runtime_state.scheduler_steps,
      analytics.chunks_done, analytics.next_value);
  fflush(g_metrics_file);
}

static const char *priority_name(int8_t priority) {
  switch (priority) {
  case FSScheduler_Priority_BACKGROUND:
    return "background";
  case FSScheduler_Priority_UI:
    return "ui";
  case FSScheduler_Priority_INTERACTIVE:
    return "interactive";
  default:
    return "unknown";
  }
}

static void announce_task_run(const char *task_name, int8_t priority,
                              const char *details) {
  printf("[run:%s] %s", priority_name(priority), task_name);
  if (details != NULL && details[0] != '\0') {
    printf(" | %s", details);
  }
  printf("\n");
}

static void print_title(const char *title) {
  puts(title);
  puts("============================================================");
}

static int scheduler_bg_budget(const FSScheduler *scheduler) {
  return (int)(scheduler->budgetsPacked & 0x01u);
}

static int scheduler_ui_budget(const FSScheduler *scheduler) {
  return (int)((scheduler->budgetsPacked >> 1) & 0x03u);
}

static int scheduler_interactive_budget(const FSScheduler *scheduler) {
  return (int)((scheduler->budgetsPacked >> 3) & 0x07u);
}

static void print_scheduler_snapshot(const struct Feather *feather,
                                     const char *label) {
  const FSScheduler *scheduler = &feather->scheduler;

  printf("   [state] %s | queued I:%d U:%d B:%d | budget I:%d U:%d B:%d\n",
         label, scheduler->interactiveCount, scheduler->uiCount,
         scheduler->bgCount, scheduler_interactive_budget(scheduler),
         scheduler_ui_budget(scheduler), scheduler_bg_budget(scheduler));
}

static void print_process_snapshot(const char *label) {
  ProcessSnapshot snapshot;

  collect_process_snapshot(&snapshot);
  write_metrics_row(&snapshot);

  printf("[monitor] %s\n", label);
  printf(
      "[monitor] pid=%d ppid=%d | wall=%.3fs | proc_cpu=%.3fs | cpu=%.1f%%\n",
      (int)snapshot.pid, (int)snapshot.ppid, snapshot.wall_seconds,
      snapshot.proc_cpu_seconds, snapshot.cpu_percent);
  printf("[monitor] rss=%ld KB | hwm=%ld KB | vm=%ld KB | threads=%ld | "
         "open_fds=%d\n",
         snapshot.rss_kb, snapshot.hwm_kb, snapshot.vm_size_kb,
         snapshot.threads, snapshot.open_fds);
  printf("[monitor] ctxt voluntary=%ld | involuntary=%ld | steps=%lld | "
         "chunks=%d/%d | next=%d\n",
         snapshot.voluntary_ctxt, snapshot.involuntary_ctxt,
         runtime_state.scheduler_steps, analytics.chunks_done,
         analytics.total_chunks, analytics.next_value);
  if (active_feather != NULL) {
    print_scheduler_snapshot(active_feather, "monitor snapshot");
  }
}

static void redraw_live_status_line(const char *phase) {
  ProcessSnapshot snapshot;
  char line[512];
  int offset = 0;

  collect_process_snapshot(&snapshot);
  write_metrics_row(&snapshot);

  offset += snprintf(line + offset, sizeof(line) - (size_t)offset,
                     "\r[live] %-10s | wall=%6.2fs cpu=%6.2fs (%5.1f%%) | "
                     "rss=%7ld KB hwm=%7ld KB | thr=%ld fd=%d | steps=%lld",
                     phase, snapshot.wall_seconds, snapshot.proc_cpu_seconds,
                     snapshot.cpu_percent, snapshot.rss_kb, snapshot.hwm_kb,
                     snapshot.threads, snapshot.open_fds,
                     runtime_state.scheduler_steps);

  if (active_feather != NULL) {
    const FSScheduler *scheduler = &active_feather->scheduler;
    snprintf(line + offset, sizeof(line) - (size_t)offset,
             " | q[I:%d U:%d B:%d] | budget[I:%d U:%d B:%d] | chunk=%d/%d",
             scheduler->interactiveCount, scheduler->uiCount,
             scheduler->bgCount, scheduler_interactive_budget(scheduler),
             scheduler_ui_budget(scheduler), scheduler_bg_budget(scheduler),
             analytics.chunks_done, analytics.total_chunks);
  }

  fputs(line, stdout);
  fflush(stdout);
  g_live_line_visible = 1;
}

static bool feather_has_pending_tasks(const struct Feather *feather) {
  return Feather_has_pending_tasks(feather);
}

static void print_controls(void) {
  status_puts(
      "controls: /r redraw line, /p process snapshot, /l log path, /q stop");
}

static void handle_command(const char *line) {
  if (strcmp(line, "/r") == 0) {
    redraw_live_status_line("manual");
    print_process_snapshot("manual redraw");
  } else if (strcmp(line, "/p") == 0) {
    print_process_snapshot("manual snapshot");
  } else if (strcmp(line, "/l") == 0) {
    status_printf("[log] text=%s\n", g_log_path[0] ? g_log_path : "(disabled)");
    status_printf("[log] csv=%s\n",
                  g_metrics_path[0] ? g_metrics_path : "(disabled)");
  } else if (strcmp(line, "/q") == 0) {
    runtime_state.stop_requested = true;
    status_puts("[control] stop requested by user");
  } else if (line[0] != '\0') {
    status_printf("[control] unknown command: %s\n", line);
    print_controls();
  }
}

static void poll_console_commands(void) {
  fd_set readfds;
  struct timeval tv;
  char line[256];

  FD_ZERO(&readfds);
  FD_SET(STDIN_FILENO, &readfds);
  tv.tv_sec = 0;
  tv.tv_usec = 0;

  if (select(STDIN_FILENO + 1, &readfds, NULL, NULL, &tv) <= 0) {
    return;
  }

  if (fgets(line, sizeof(line), stdin) == NULL) {
    return;
  }

  line[strcspn(line, "\r\n")] = '\0';
  handle_command(line);
}

static void maybe_refresh_runtime_views(const char *phase) {
  double now = monotonic_seconds();

  poll_console_commands();

  if (now - runtime_state.last_live_refresh >= LIVE_REFRESH_INTERVAL_SEC) {
    redraw_live_status_line(phase);
    runtime_state.last_live_refresh = now;
  }

  if (now - runtime_state.last_snapshot >= SNAPSHOT_INTERVAL_SEC) {
    print_process_snapshot(phase);
    runtime_state.last_snapshot = now;
  }
}

static bool schedule_task(void (*task_fn)(void *context), int8_t priority,
                          const char *label) {
  FSSchedulerInstantTask task = {.task = task_fn, .priority = priority};

  if (!Feather_add_instant_task(active_feather, task)) {
    demo_failed = true;
    status_printf("[error] failed to schedule \"%s\" on the %s queue\n", label,
                  priority_name(priority));
    return false;
  }

  printf("   [schedule] \"%s\" -> %s queue, start now\n", label,
         priority_name(priority));
  print_scheduler_snapshot(active_feather, "after schedule");
  return true;
}

static void drain_ready_tasks(struct Feather *feather, int max_steps) {
  print_scheduler_snapshot(feather, "before drain");

  while (runtime_state.scheduler_steps < max_steps) {
    bool stepped;

    maybe_refresh_runtime_views("scheduler");
    if (runtime_state.stop_requested) {
      demo_failed = true;
      break;
    }

    stepped = Feather_step(feather);
    if (stepped) {
      runtime_state.scheduler_steps++;
      print_scheduler_snapshot(feather, "after step");
      continue;
    }

    if (!feather_has_pending_tasks(feather)) {
      break;
    }

    {
      struct timespec ts;
      uint64_t sleep_ms = 0;
      if (Feather_next_sleep_ms(feather, &sleep_ms) && sleep_ms > 0) {
        ts.tv_sec = (time_t)(sleep_ms / 1000);
        ts.tv_nsec = (long)((sleep_ms % 1000) * 1000000ULL);
      } else {
        ts.tv_sec = 0;
        ts.tv_nsec = IDLE_SLEEP_NSEC;
      }
      nanosleep(&ts, NULL);
    }
  }

  clear_live_line_if_needed();

  if (runtime_state.scheduler_steps >= max_steps &&
      feather_has_pending_tasks(feather)) {
    status_puts(
        "[scheduler] stopped after the safety limit while draining tasks");
    demo_failed = true;
  }
}

static bool is_prime(int value) {
  int divisor;

  if (value < 2) {
    return false;
  }

  for (divisor = 2; divisor * divisor <= value; divisor++) {
    if (value % divisor == 0) {
      return false;
    }
  }

  return true;
}

static int sum_of_divisors(int value) {
  int divisor;
  int total = 0;

  for (divisor = 1; (long long)divisor * divisor <= value; divisor++) {
    if (value % divisor == 0) {
      total += divisor;
      if (divisor != value / divisor) {
        total += value / divisor;
      }
    }
  }

  return total;
}

static long long checksum_step(long long checksum, long long contribution) {
  long long next = ((checksum * 911382323LL) + contribution) % CHECKSUM_MOD;

  if (next < 0) {
    next += CHECKSUM_MOD;
  }

  return next;
}

static long long mulmod_ll(long long a, long long b, long long mod) {
  long long result = 0;

  a %= mod;
  while (b > 0) {
    if (b & 1LL) {
      result += a;
      if (result >= mod) {
        result -= mod;
      }
    }
    if (a >= mod - a) {
      a = a - (mod - a);
    } else {
      a = a + a;
    }
    b >>= 1;
  }
  return result;
}

static long long checksum2_step(long long checksum, long long contribution) {
  const long long multiplier = 6364136223846793005LL;
  long long contrib_mod = contribution % CHECKSUM_MOD2;
  long long next;

  if (contrib_mod < 0) {
    contrib_mod += CHECKSUM_MOD2;
  }
  next = (mulmod_ll(checksum, multiplier, CHECKSUM_MOD2) + contrib_mod) %
         CHECKSUM_MOD2;
  return next;
}

static long long sum_1_to_n(int n) {
  long long x = n;
  return x * (x + 1LL) / 2LL;
}

static long long sum_squares_1_to_n(int n) {
  long long x = n;
  return x * (x + 1LL) * (2LL * x + 1LL) / 6LL;
}

static long long sum_cubes_1_to_n(int n) {
  long long triangular = sum_1_to_n(n);
  return triangular * triangular;
}

static long long sum_fourths_1_to_n(int n) {
  long long x = n;
  return x * (x + 1LL) * (2LL * x + 1LL) * (3LL * x * x + 3LL * x - 1LL) / 30LL;
}

static long long sum_fifths_1_to_n(int n) {
  long long x = n;
  long long n_sq = x * x;
  long long n1_sq = (x + 1LL) * (x + 1LL);
  long long poly = 2LL * x * x + 2LL * x - 1LL;

  return n_sq * n1_sq * poly / 12LL;
}

static int euler_totient(int n) {
  int result = n;
  int temp = n;
  int p;

  for (p = 2; p * p <= temp; p++) {
    if (temp % p == 0) {
      while (temp % p == 0) {
        temp /= p;
      }
      result -= result / p;
    }
  }
  if (temp > 1) {
    result -= result / temp;
  }
  return result;
}

static int collatz_steps(int n) {
  long long x = n;
  int steps = 0;

  while (x != 1) {
    x = (x % 2 == 0) ? x / 2 : 3LL * x + 1LL;
    steps++;
  }
  return steps;
}

static long long range_power_sum(long long (*formula)(int), int start,
                                 int end) {
  if (end < start) {
    return 0;
  }
  return formula(end) - formula(start - 1);
}

static void analytics_progress_task(void *context) {
  (void)context;
  char details[160];
  int processed = analytics.next_value - analytics.start_value;

  snprintf(details, sizeof(details), "publish progress after chunk %d of %d",
           analytics.chunks_done, analytics.total_chunks);
  announce_task_run("analytics_progress_task", FSScheduler_Priority_UI,
                    details);
  printf("[ui] processed %d/%lld values | sum=%lld | sum_squares=%lld | "
         "checksum=%lld\n",
         processed, analytics.count, analytics.sum, analytics.sum_squares,
         analytics.checksum);
  printf(
      "[ui] prime_count=%lld | divisor_total=%lld | residue_signature=%lld\n",
      analytics.prime_count, analytics.divisor_total,
      analytics.residue_signature);
  printf(
      "[ui] euler_totient_total=%lld | collatz_total=%lld | checksum2=%lld\n",
      analytics.euler_totient_total, analytics.collatz_total,
      analytics.checksum2);
}

static void analytics_done_task(void *context) {
  (void)context;
  announce_task_run("analytics_done_task", FSScheduler_Priority_INTERACTIVE,
                    "announce that the complex analytics pipeline is complete");
  puts("[interactive] complex analytics pipeline finished");
}

static void analytics_result_task(void *context) {
  (void)context;
  char details[160];

  snprintf(details, sizeof(details),
           "render final analytics summary for values %d..%d",
           analytics.start_value, analytics.end_value);
  announce_task_run("analytics_result_task", FSScheduler_Priority_UI, details);

  printf("[ui] final raw totals\n");
  printf("[ui] sum=%lld | sum_squares=%lld | sum_cubes=%lld | sum_fourths=%lld "
         "| sum_fifths=%lld\n",
         analytics.sum, analytics.sum_squares, analytics.sum_cubes,
         analytics.sum_fourths, analytics.sum_fifths);
  printf("[ui] weighted_polynomial=%lld | alternating_cubic_balance=%lld\n",
         analytics.weighted_polynomial, analytics.alternating_cubic_balance);
  printf("[ui] divisor_total=%lld | prime_count=%lld | prime_cube_total=%lld\n",
         analytics.divisor_total, analytics.prime_count,
         analytics.prime_cube_total);
  printf("[ui] euler_totient_total=%lld | collatz_total=%lld\n",
         analytics.euler_totient_total, analytics.collatz_total);
  printf("[ui] residue_signature=%lld | checksum=%lld | checksum2=%lld\n",
         analytics.residue_signature, analytics.checksum, analytics.checksum2);

  printf("[ui] derived moments\n");
  printf("[ui] mean=%.4Lf | variance=%.4Lf\n", analytics.mean,
         analytics.variance);
  printf("[ui] third_central_moment=%.4Lf | fourth_central_moment=%.4Lf\n",
         analytics.third_central_moment, analytics.fourth_central_moment);
  printf("[ui] raw_fifth_moment=%.4Lf\n", analytics.raw_fifth_moment);
  printf("[ui] prime_density=%.6Lf | normalized_energy=%.4Lf | "
         "stability_index=%.6Lf\n",
         analytics.prime_density, analytics.normalized_energy,
         analytics.stability_index);

  printf("[ui] validation\n");
  printf("[ui] sum: %s | expected=%lld actual=%lld\n",
         analytics.sum_valid ? "PASS" : "FAIL", analytics.expected_sum,
         analytics.sum);
  printf("[ui] sum_squares: %s | expected=%lld actual=%lld\n",
         analytics.sum_squares_valid ? "PASS" : "FAIL",
         analytics.expected_sum_squares, analytics.sum_squares);
  printf("[ui] sum_cubes: %s | expected=%lld actual=%lld\n",
         analytics.sum_cubes_valid ? "PASS" : "FAIL",
         analytics.expected_sum_cubes, analytics.sum_cubes);
  printf("[ui] sum_fourths: %s | expected=%lld actual=%lld\n",
         analytics.sum_fourths_valid ? "PASS" : "FAIL",
         analytics.expected_sum_fourths, analytics.sum_fourths);
  printf("[ui] sum_fifths: %s | expected=%lld actual=%lld\n",
         analytics.sum_fifths_valid ? "PASS" : "FAIL",
         analytics.expected_sum_fifths, analytics.sum_fifths);

  schedule_task(analytics_done_task, FSScheduler_Priority_INTERACTIVE,
                "analytics done");
}

static void analytics_validate_task(void *context) {
  (void)context;
  announce_task_run("analytics_validate_task", FSScheduler_Priority_BACKGROUND,
                    "validate raw power sums against closed-form formulas");

  analytics.expected_sum =
      range_power_sum(sum_1_to_n, analytics.start_value, analytics.end_value);
  analytics.expected_sum_squares = range_power_sum(
      sum_squares_1_to_n, analytics.start_value, analytics.end_value);
  analytics.expected_sum_cubes = range_power_sum(
      sum_cubes_1_to_n, analytics.start_value, analytics.end_value);
  analytics.expected_sum_fourths = range_power_sum(
      sum_fourths_1_to_n, analytics.start_value, analytics.end_value);
  analytics.expected_sum_fifths = range_power_sum(
      sum_fifths_1_to_n, analytics.start_value, analytics.end_value);

  analytics.sum_valid = analytics.sum == analytics.expected_sum;
  analytics.sum_squares_valid =
      analytics.sum_squares == analytics.expected_sum_squares;
  analytics.sum_cubes_valid =
      analytics.sum_cubes == analytics.expected_sum_cubes;
  analytics.sum_fourths_valid =
      analytics.sum_fourths == analytics.expected_sum_fourths;
  analytics.sum_fifths_valid =
      analytics.sum_fifths == analytics.expected_sum_fifths;

  if (!analytics.sum_valid || !analytics.sum_squares_valid ||
      !analytics.sum_cubes_valid || !analytics.sum_fourths_valid ||
      !analytics.sum_fifths_valid) {
    demo_failed = true;
  }

  printf("[background] validation checks prepared for %d..%d\n",
         analytics.start_value, analytics.end_value);
  schedule_task(analytics_result_task, FSScheduler_Priority_UI,
                "analytics result");
}

static void analytics_reduce_indices_task(void *context) {
  (void)context;
  announce_task_run(
      "analytics_reduce_indices_task", FSScheduler_Priority_BACKGROUND,
      "derive higher-level indices from raw moments and signatures");

  analytics.prime_density =
      (long double)analytics.prime_count / (long double)analytics.count;
  analytics.normalized_energy =
      ((long double)analytics.weighted_polynomial +
       (long double)analytics.alternating_cubic_balance +
       (long double)analytics.prime_cube_total +
       (long double)analytics.divisor_total +
       (long double)analytics.residue_signature +
       (long double)analytics.euler_totient_total +
       (long double)analytics.collatz_total) /
      (long double)analytics.count;

  analytics.stability_index =
      (analytics.fourth_central_moment / (1.0L + analytics.variance)) +
      ((analytics.third_central_moment * analytics.third_central_moment) /
       (1.0L + analytics.fourth_central_moment)) +
      (analytics.prime_density * 100.0L) +
      ((long double)analytics.checksum / (long double)CHECKSUM_MOD) +
      ((long double)analytics.checksum2 / (long double)CHECKSUM_MOD2);

  printf("[background] indices ready | prime_density=%.6Lf | "
         "normalized_energy=%.4Lf | stability_index=%.6Lf\n",
         analytics.prime_density, analytics.normalized_energy,
         analytics.stability_index);

  schedule_task(analytics_validate_task, FSScheduler_Priority_BACKGROUND,
                "analytics validate");
}

static void analytics_reduce_moments_task(void *context) {
  (void)context;
  announce_task_run("analytics_reduce_moments_task",
                    FSScheduler_Priority_BACKGROUND,
                    "reduce raw power sums into central moments and variance");

  analytics.mean = (long double)analytics.sum / (long double)analytics.count;
  analytics.raw_second_moment =
      (long double)analytics.sum_squares / (long double)analytics.count;
  analytics.raw_third_moment =
      (long double)analytics.sum_cubes / (long double)analytics.count;
  analytics.raw_fourth_moment =
      (long double)analytics.sum_fourths / (long double)analytics.count;
  analytics.raw_fifth_moment =
      (long double)analytics.sum_fifths / (long double)analytics.count;

  analytics.variance =
      analytics.raw_second_moment - (analytics.mean * analytics.mean);
  analytics.third_central_moment =
      analytics.raw_third_moment -
      (3.0L * analytics.mean * analytics.raw_second_moment) +
      (2.0L * analytics.mean * analytics.mean * analytics.mean);
  analytics.fourth_central_moment =
      analytics.raw_fourth_moment -
      (4.0L * analytics.mean * analytics.raw_third_moment) +
      (6.0L * analytics.mean * analytics.mean * analytics.raw_second_moment) -
      (3.0L * analytics.mean * analytics.mean * analytics.mean *
       analytics.mean);

  printf("[background] moments ready | mean=%.4Lf | variance=%.4Lf | "
         "third=%.4Lf | fourth=%.4Lf | fifth_raw=%.4Lf\n",
         analytics.mean, analytics.variance, analytics.third_central_moment,
         analytics.fourth_central_moment, analytics.raw_fifth_moment);

  schedule_task(analytics_reduce_indices_task, FSScheduler_Priority_BACKGROUND,
                "analytics reduce indices");
}

static void analytics_chunk_task(void *context) {
  (void)context;
  int start = analytics.next_value;
  int end = start + analytics.chunk_size - 1;
  long long chunk_sum = 0;
  long long chunk_squares = 0;
  long long chunk_cubes = 0;
  long long chunk_fourths = 0;
  long long chunk_fifths = 0;
  long long chunk_polynomial = 0;
  long long chunk_alternating = 0;
  long long chunk_divisors = 0;
  long long chunk_prime_count = 0;
  long long chunk_prime_cubes = 0;
  long long chunk_residue = 0;
  long long chunk_euler = 0;
  long long chunk_collatz = 0;
  long long chunk_checksum = analytics.checksum;
  long long chunk_checksum2 = analytics.checksum2;
  int value;
  char details[192];

  if (end > analytics.end_value) {
    end = analytics.end_value;
  }

  snprintf(details, sizeof(details), "process chunk %d/%d over values %d..%d",
           analytics.chunks_done + 1, analytics.total_chunks, start, end);
  announce_task_run("analytics_chunk_task", FSScheduler_Priority_BACKGROUND,
                    details);

  for (value = start; value <= end; value++) {
    long long x = value;
    long long square = x * x;
    long long cube = square * x;
    long long fourth = square * square;
    long long fifth = fourth * x;
    long long triangular = x * (x + 1LL) / 2LL;
    long long divisors = sum_of_divisors(value);
    long long etot = euler_totient(value);
    long long csteps = collatz_steps(value);
    long long polynomial =
        (7LL * fourth) - (5LL * cube) + (11LL * square) - (13LL * x) + 17LL;
    long long alternating = (value % 2 == 0)
                                ? (cube + (3LL * square) - (2LL * x))
                                : -((cube - (2LL * square) + x));
    long long residue = ((square + (3LL * x) + 7LL) % 11LL) *
                        ((cube + x + divisors + triangular) % 13LL);

    chunk_sum += x;
    chunk_squares += square;
    chunk_cubes += cube;
    chunk_fourths += fourth;
    chunk_fifths += fifth;
    chunk_polynomial += polynomial;
    chunk_alternating += alternating;
    chunk_divisors += divisors;
    chunk_residue += residue;
    chunk_euler += etot;
    chunk_collatz += csteps;

    if (is_prime(value)) {
      chunk_prime_count++;
      chunk_prime_cubes += cube + triangular;
    }

    chunk_checksum =
        checksum_step(chunk_checksum, polynomial + alternating + divisors +
                                          residue + triangular);
    chunk_checksum2 = checksum2_step(chunk_checksum2, fifth + etot + csteps +
                                                          residue + triangular);
  }

  analytics.sum += chunk_sum;
  analytics.sum_squares += chunk_squares;
  analytics.sum_cubes += chunk_cubes;
  analytics.sum_fourths += chunk_fourths;
  analytics.sum_fifths += chunk_fifths;
  analytics.weighted_polynomial += chunk_polynomial;
  analytics.alternating_cubic_balance += chunk_alternating;
  analytics.divisor_total += chunk_divisors;
  analytics.prime_count += chunk_prime_count;
  analytics.prime_cube_total += chunk_prime_cubes;
  analytics.residue_signature += chunk_residue;
  analytics.euler_totient_total += chunk_euler;
  analytics.collatz_total += chunk_collatz;
  analytics.checksum = chunk_checksum;
  analytics.checksum2 = chunk_checksum2;
  analytics.next_value = end + 1;
  analytics.chunks_done++;

  printf("[background] chunk %d summary\n", analytics.chunks_done);
  printf("[background] raw: sum=%lld | squares=%lld | cubes=%lld | "
         "fourths=%lld | fifths=%lld\n",
         chunk_sum, chunk_squares, chunk_cubes, chunk_fourths, chunk_fifths);
  printf("[background] derived: polynomial=%lld | alternating=%lld | "
         "divisors=%lld\n",
         chunk_polynomial, chunk_alternating, chunk_divisors);
  printf("[background] number-theory: euler=%lld | collatz=%lld\n", chunk_euler,
         chunk_collatz);
  printf("[background] signatures: prime_count=%lld | prime_cube_total=%lld | "
         "residue=%lld\n",
         chunk_prime_count, chunk_prime_cubes, chunk_residue);
  printf("[background] checksums: checksum=%lld | checksum2=%lld\n",
         chunk_checksum, chunk_checksum2);
  printf("[background] totals: sum=%lld | squares=%lld | cubes=%lld | "
         "fourths=%lld | fifths=%lld\n",
         analytics.sum, analytics.sum_squares, analytics.sum_cubes,
         analytics.sum_fourths, analytics.sum_fifths);

  if (analytics.next_value <= analytics.end_value) {
    schedule_task(analytics_progress_task, FSScheduler_Priority_UI,
                  "analytics progress");
    schedule_task(analytics_chunk_task, FSScheduler_Priority_BACKGROUND,
                  "next analytics chunk");
  } else {
    schedule_task(analytics_reduce_moments_task,
                  FSScheduler_Priority_BACKGROUND, "analytics reduce moments");
  }
}

static void reset_analytics(void) {
  memset(&analytics, 0, sizeof(analytics));
  analytics.start_value = 1;
  analytics.end_value = 500;
  analytics.next_value = 1;
  analytics.chunk_size = 50;
  analytics.chunks_done = 0;
  analytics.total_chunks = 10;
  analytics.count = analytics.end_value - analytics.start_value + 1;
}

static void demo_complex_analytics(void) {
  struct Feather feather;

  print_title("Feather Complex Calculation Demo");
  puts("calculation-only pipeline:");
  puts("- process values 1..500 in 10 chunks of 50 values");
  puts("- accumulate raw power sums up to x^5");
  puts("- accumulate polynomial, alternating cubic, divisor, prime, residue, "
       "and checksum signatures");
  puts("- accumulate Euler totient sums and Collatz step counts");
  puts("- maintain two independent checksums; checksum2 uses mulmod to avoid "
       "__int128");
  puts("- reduce raw sums into variance and higher central moments including "
       "the raw fifth moment");
  puts("- derive normalized energy, prime density, and a composite stability "
       "index");
  puts("- validate power sums against closed-form formulas");
  puts("");

  if (!Feather_init(&feather)) {
    status_puts("[error] Feather_init failed");
    demo_failed = true;
    return;
  }

  active_feather = &feather;
  print_scheduler_snapshot(&feather, "after init");

  reset_analytics();

  schedule_task(analytics_chunk_task, FSScheduler_Priority_BACKGROUND,
                "first analytics chunk");
  drain_ready_tasks(&feather, SAFETY_MAX_STEPS);

  print_process_snapshot("after scheduler");

  Feather_deinit(&feather);
  active_feather = NULL;
}

static void perform_additional_computation(double target_seconds) {
  uint64_t state = 0x9E3779B97F4A7C15ULL;
  uint64_t accumulator = 0;
  double start = monotonic_seconds();

  puts("");
  puts("[tail] starting heavy tail computation");

  while ((monotonic_seconds() - start) < target_seconds &&
         !runtime_state.stop_requested) {
    int i;

    for (i = 0; i < 200000; i++) {
      state ^= state >> 12;
      state ^= state << 25;
      state ^= state >> 27;
      state *= 2685821657736338717ULL;
      accumulator ^= state + (uint64_t)i;
    }

    runtime_state.tail_iterations += 200000ULL;
    maybe_refresh_runtime_views("tail");
  }

  clear_live_line_if_needed();
  printf("[tail] heavy computation finished | iterations=%llu | "
         "accumulator=%llu\n",
         runtime_state.tail_iterations, (unsigned long long)accumulator);
}

int main(void) {
  runtime_state.wall_start = monotonic_seconds();
  runtime_state.cpu_start = process_cpu_seconds();
  runtime_state.last_live_refresh = runtime_state.wall_start;
  runtime_state.last_snapshot = runtime_state.wall_start;
  runtime_state.last_cpu_sample = runtime_state.cpu_start;
  runtime_state.last_wall_sample = runtime_state.wall_start;
  runtime_state.last_cpu_percent = 0.0;
  runtime_state.scheduler_steps = 0;
  runtime_state.tail_iterations = 0;
  runtime_state.stop_requested = false;

  initialize_logs();

  if (g_log_path[0] != '\0') {
    status_printf("[log] text=%s\n", g_log_path);
  }
  if (g_metrics_path[0] != '\0') {
    status_printf("[log] csv=%s\n", g_metrics_path);
  }
  print_controls();
  print_process_snapshot("startup");

  demo_complex_analytics();

  if (!demo_failed && !runtime_state.stop_requested) {
    perform_additional_computation(TAIL_CPU_SECONDS);
  }

  print_process_snapshot("shutdown");

  if (g_metrics_file != NULL) {
    fclose(g_metrics_file);
    g_metrics_file = NULL;
  }
  if (g_log_file != NULL) {
    fclose(g_log_file);
    g_log_file = NULL;
  }

  puts("");
  status_puts("Complex calculation demo complete.");

  if (demo_failed || runtime_state.stop_requested) {
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
