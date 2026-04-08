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
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <libproc.h>
#include <mach/mach.h>
#include <mach/task_info.h>
#endif

#include "Feather.hpp"

#define FAST_CHECKSUM_MOD 1000000007ULL
#define FAST_SAMPLE_INTERVAL_SEC 0.10
#define FAST_PROFILE_SECONDS 1.20
#define FAST_IDLE_SLEEP_NSEC 1000000L
#define FAST_SAFETY_MAX_STEPS 256
#define FAST_START_VALUE 1
#define FAST_END_VALUE 96
#define FAST_CHUNK_SIZE 24

typedef struct ProcessSnapshot {
  char phase[24];
  double wall_seconds;
  double proc_cpu_seconds;
  double user_cpu_seconds;
  double sys_cpu_seconds;
  double wall_delta_seconds;
  double cpu_delta_seconds;
  double cpu_percent;
  long rss_kb;
  long hwm_kb;
  long vm_kb;
  long footprint_kb;
  long threads;
  long open_fds;
  long voluntary_ctxt;
  long involuntary_ctxt;
  long minor_faults;
  long major_faults;
  long pageins;
  long messages_sent;
  long messages_received;
  long block_inputs;
  long block_outputs;
  long faults_total;
  long cow_faults;
  long unix_syscalls;
  long mach_syscalls;
  long task_csw_total;
  pid_t pid;
  pid_t ppid;
  int interactive_count;
  int ui_count;
  int bg_count;
  int interactive_budget;
  int ui_budget;
  int bg_budget;
  long long scheduler_steps;
  unsigned long long work_units;
} ProcessSnapshot;

typedef struct MetricsSummary {
  unsigned long long sample_count;
  double cpu_percent_sum;
  double peak_cpu_percent;
  char peak_cpu_phase[24];
  long peak_rss_kb;
  long peak_hwm_kb;
  long peak_footprint_kb;
  long peak_vm_kb;
  long peak_threads;
  long peak_open_fds;
  int peak_queue_depth;
  long long total_vcsw_delta;
  long long total_ivcsw_delta;
  long long total_minor_fault_delta;
  long long total_major_fault_delta;
  long long total_pageins_delta;
  long long total_block_inputs_delta;
  long long total_block_outputs_delta;
  long long total_messages_sent_delta;
  long long total_messages_received_delta;
  ProcessSnapshot previous;
  bool has_previous;
} MetricsSummary;

typedef struct RuntimeState {
  double wall_start;
  double cpu_start;
  double last_emit_wall;
  double last_cpu_sample;
  double last_wall_sample;
  double last_cpu_percent;
  long long scheduler_steps;
  unsigned long long work_units;
  bool stop_requested;
} RuntimeState;

typedef struct FastWorkload {
  int start_value;
  int end_value;
  int next_value;
  int chunk_size;
  int chunks_done;
  int total_chunks;
  long long sum;
  long long sum_squares;
  long long weighted_sum;
  long long even_count;
  long long odd_count;
  long long popcount_total;
  uint64_t checksum;
  bool completed;
} FastWorkload;

static struct Feather *active_feather = NULL;
static FILE *g_log_file = NULL;
static FILE *g_metrics_file = NULL;
static char g_log_path[256] = {0};
static char g_metrics_path[256] = {0};
static bool g_stdout_is_tty = false;
static int g_live_line_visible = 0;
static bool demo_failed = false;

static RuntimeState runtime_state;
static MetricsSummary summary_state;
static FastWorkload fast_workload;

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

static double timeval_seconds(struct timeval value) {
  return (double)value.tv_sec + (double)value.tv_usec / 1000000.0;
}

static void clear_live_line_if_needed(void) {
  if (g_live_line_visible && g_stdout_is_tty) {
    fputs("\r\033[2K", stdout);
    fflush(stdout);
    g_live_line_visible = 0;
  }
}

static void finish_live_line(void) {
  if (g_live_line_visible && g_stdout_is_tty) {
    fputc('\n', stdout);
    fflush(stdout);
    g_live_line_visible = 0;
  }
}

static size_t stdout_columns(void) {
  struct winsize ws;

  if (!g_stdout_is_tty) {
    return 0;
  }

  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0 || ws.ws_col == 0) {
    return 0;
  }

  return (size_t)ws.ws_col;
}

static void append_line_fragment(char *line, size_t line_size, size_t *offset,
                                 const char *fmt, ...) {
  va_list ap;
  int written;

  if (*offset >= line_size) {
    return;
  }

  va_start(ap, fmt);
  written = vsnprintf(line + *offset, line_size - *offset, fmt, ap);
  va_end(ap);

  if (written < 0) {
    return;
  }

  if ((size_t)written >= line_size - *offset) {
    *offset = line_size - 1;
    return;
  }

  *offset += (size_t)written;
}

static int log_vprintf(const char *fmt, va_list ap) {
  if (g_log_file == NULL) {
    return 0;
  }

  if (vfprintf(g_log_file, fmt, ap) < 0) {
    return EOF;
  }
  fflush(g_log_file);
  return 0;
}

static int log_printf(const char *fmt, ...) {
  va_list ap;
  int rc;

  va_start(ap, fmt);
  rc = log_vprintf(fmt, ap);
  va_end(ap);
  return rc;
}

static int log_puts(const char *s) {
  if (g_log_file == NULL) {
    return 0;
  }

  if (fputs(s, g_log_file) == EOF || fputc('\n', g_log_file) == EOF) {
    return EOF;
  }
  fflush(g_log_file);
  return 0;
}

static int error_printf(const char *fmt, ...) {
  va_list ap;
  va_list ap_copy;
  int rc;

  clear_live_line_if_needed();
  va_start(ap, fmt);
  va_copy(ap_copy, ap);
  rc = vfprintf(stderr, fmt, ap);
  fflush(stderr);
  if (log_vprintf(fmt, ap_copy) == EOF) {
    rc = EOF;
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
  snprintf(g_log_path, sizeof(g_log_path), "logs/fast_demo_run_%s.log", stamp);
  snprintf(g_metrics_path, sizeof(g_metrics_path),
           "logs/fast_demo_metrics_%s.csv", stamp);

  g_log_file = fopen(g_log_path, "w");
  g_metrics_file = fopen(g_metrics_path, "w");

  if (g_metrics_file != NULL) {
    fprintf(g_metrics_file, "phase,wall_s,proc_cpu_s,user_cpu_s,sys_cpu_s,wall_"
                            "delta_s,cpu_delta_s,cpu_pct,"
                            "rss_kb,hwm_kb,vm_kb,footprint_kb,threads,open_fds,"
                            "voluntary_ctxt,involuntary_ctxt,"
                            "minor_faults,major_faults,pageins,messages_sent,"
                            "messages_received,block_inputs,block_outputs,"
                            "faults_total,cow_faults,unix_syscalls,mach_"
                            "syscalls,task_csw_total,pid,ppid,"
                            "q_interactive,q_ui,q_bg,budget_interactive,budget_"
                            "ui,budget_bg,scheduler_steps,work_units\n");
    fflush(g_metrics_file);
  }

  log_puts("Feather Fast Detailed Demo");
  log_puts("========================================");
  log_printf("[log] text=%s\n", g_log_path);
  log_printf("[log] csv=%s\n", g_metrics_path);
  log_puts(
      "[workload] compact scheduler pipeline with detailed resource sampling");
}

static long read_status_long_value_generic(const char *key) {
#if defined(__APPLE__)
  (void)key;
  return -1;
#else
  FILE *fp;
  char line[256];

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

    errno = 0;
    {
      long value = strtol(colon + 1, &endptr, 10);
      if (errno == 0 && endptr != colon + 1) {
        fclose(fp);
        return value;
      }
    }
  }

  fclose(fp);
  return -1;
#endif
}

static long bytes_to_kb(uint64_t bytes) { return (long)(bytes / 1024ULL); }

static long clamp_nonnegative_delta(long current, long previous) {
  if (current < 0 || previous < 0 || current < previous) {
    return 0;
  }
  return current - previous;
}

static int count_open_fds(void) {
#if defined(__APPLE__)
  int capacity = 32;

  while (capacity <= 4096) {
    struct proc_fdinfo *buffer;
    int bytes;
    int buffer_size = capacity * PROC_PIDLISTFD_SIZE;

    buffer = (struct proc_fdinfo *)malloc((size_t)buffer_size);
    if (buffer == NULL) {
      return -1;
    }

    bytes = proc_pidinfo(getpid(), PROC_PIDLISTFDS, 0, buffer, buffer_size);
    free(buffer);

    if (bytes < 0) {
      return -1;
    }
    if (bytes < buffer_size) {
      return bytes / PROC_PIDLISTFD_SIZE;
    }

    capacity *= 2;
  }

  return -1;
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

static int scheduler_bg_budget(const FSScheduler *scheduler) {
  return (int)(scheduler->budgetsPacked & 0x01u);
}

static int scheduler_ui_budget(const FSScheduler *scheduler) {
  return (int)((scheduler->budgetsPacked >> 1) & 0x03u);
}

static int scheduler_interactive_budget(const FSScheduler *scheduler) {
  return (int)((scheduler->budgetsPacked >> 3) & 0x07u);
}

static void collect_platform_process_metrics(ProcessSnapshot *snapshot) {
  snapshot->rss_kb = -1;
  snapshot->hwm_kb = -1;
  snapshot->vm_kb = -1;
  snapshot->footprint_kb = -1;
  snapshot->threads = -1;
  snapshot->pageins = -1;
  snapshot->faults_total = -1;
  snapshot->cow_faults = -1;
  snapshot->unix_syscalls = -1;
  snapshot->mach_syscalls = -1;
  snapshot->task_csw_total = -1;

#if defined(__APPLE__)
  {
    struct proc_taskinfo taskinfo;
    task_vm_info_data_t vm_info;
    mach_msg_type_number_t vm_info_count = TASK_VM_INFO_COUNT;
    int taskinfo_size;

    taskinfo_size = proc_pidinfo(getpid(), PROC_PIDTASKINFO, 0, &taskinfo,
                                 PROC_PIDTASKINFO_SIZE);
    if (taskinfo_size == PROC_PIDTASKINFO_SIZE) {
      snapshot->rss_kb = bytes_to_kb(taskinfo.pti_resident_size);
      snapshot->vm_kb = bytes_to_kb(taskinfo.pti_virtual_size);
      snapshot->threads = taskinfo.pti_threadnum;
      snapshot->pageins = taskinfo.pti_pageins;
      snapshot->faults_total = taskinfo.pti_faults;
      snapshot->cow_faults = taskinfo.pti_cow_faults;
      snapshot->unix_syscalls = taskinfo.pti_syscalls_unix;
      snapshot->mach_syscalls = taskinfo.pti_syscalls_mach;
      snapshot->task_csw_total = taskinfo.pti_csw;
    }

    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vm_info,
                  &vm_info_count) == KERN_SUCCESS) {
      snapshot->hwm_kb = bytes_to_kb(vm_info.resident_size_peak);
      snapshot->footprint_kb = bytes_to_kb(vm_info.phys_footprint);
    }
  }
#else
  snapshot->rss_kb = read_status_long_value_generic("VmRSS");
  snapshot->hwm_kb = read_status_long_value_generic("VmHWM");
  snapshot->vm_kb = read_status_long_value_generic("VmSize");
  snapshot->threads = read_status_long_value_generic("Threads");
#endif
}

static void capture_process_snapshot(ProcessSnapshot *snapshot,
                                     const char *phase) {
  struct rusage usage;
  double now_wall = monotonic_seconds();
  double now_cpu = process_cpu_seconds();
  double wall_delta = now_wall - runtime_state.last_wall_sample;
  double cpu_delta = now_cpu - runtime_state.last_cpu_sample;

  memset(snapshot, 0, sizeof(*snapshot));
  strncpy(snapshot->phase, phase, sizeof(snapshot->phase) - 1);
  snapshot->phase[sizeof(snapshot->phase) - 1] = '\0';
  snapshot->wall_seconds = now_wall - runtime_state.wall_start;
  snapshot->proc_cpu_seconds = now_cpu - runtime_state.cpu_start;
  snapshot->wall_delta_seconds = wall_delta;
  snapshot->cpu_delta_seconds = cpu_delta;

  if (wall_delta >= 0.001) {
    runtime_state.last_cpu_percent = (cpu_delta / wall_delta) * 100.0;
  } else if (runtime_state.scheduler_steps == 0 &&
             runtime_state.work_units == 0) {
    runtime_state.last_cpu_percent = 0.0;
  }
  snapshot->cpu_percent = runtime_state.last_cpu_percent;

  collect_platform_process_metrics(snapshot);
  snapshot->open_fds = count_open_fds();
  snapshot->pid = getpid();
  snapshot->ppid = getppid();
  snapshot->scheduler_steps = runtime_state.scheduler_steps;
  snapshot->work_units = runtime_state.work_units;

  if (active_feather != NULL) {
    const FSScheduler *scheduler = &active_feather->scheduler;

    snapshot->interactive_count = scheduler->interactiveCount;
    snapshot->ui_count = scheduler->uiCount;
    snapshot->bg_count = scheduler->bgCount;
    snapshot->interactive_budget = scheduler_interactive_budget(scheduler);
    snapshot->ui_budget = scheduler_ui_budget(scheduler);
    snapshot->bg_budget = scheduler_bg_budget(scheduler);
  } else {
    snapshot->interactive_count = 0;
    snapshot->ui_count = 0;
    snapshot->bg_count = 0;
    snapshot->interactive_budget = 0;
    snapshot->ui_budget = 0;
    snapshot->bg_budget = 0;
  }

  if (getrusage(RUSAGE_SELF, &usage) == 0) {
    snapshot->user_cpu_seconds = timeval_seconds(usage.ru_utime);
    snapshot->sys_cpu_seconds = timeval_seconds(usage.ru_stime);
    snapshot->voluntary_ctxt = usage.ru_nvcsw;
    snapshot->involuntary_ctxt = usage.ru_nivcsw;
    snapshot->minor_faults = usage.ru_minflt;
    snapshot->major_faults = usage.ru_majflt;
    snapshot->messages_sent = usage.ru_msgsnd;
    snapshot->messages_received = usage.ru_msgrcv;
    snapshot->block_inputs = usage.ru_inblock;
    snapshot->block_outputs = usage.ru_oublock;
  } else {
    snapshot->user_cpu_seconds = -1.0;
    snapshot->sys_cpu_seconds = -1.0;
    snapshot->voluntary_ctxt = -1;
    snapshot->involuntary_ctxt = -1;
    snapshot->minor_faults = -1;
    snapshot->major_faults = -1;
    snapshot->messages_sent = -1;
    snapshot->messages_received = -1;
    snapshot->block_inputs = -1;
    snapshot->block_outputs = -1;
  }

  runtime_state.last_wall_sample = now_wall;
  runtime_state.last_cpu_sample = now_cpu;
}

static void write_metrics_row(const ProcessSnapshot *snapshot) {
  if (g_metrics_file == NULL) {
    return;
  }

  fprintf(g_metrics_file,
          "%s,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.2f,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%"
          "ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%d,%d,%d,%d,%d,%"
          "d,%d,%d,%lld,%llu\n",
          snapshot->phase, snapshot->wall_seconds, snapshot->proc_cpu_seconds,
          snapshot->user_cpu_seconds, snapshot->sys_cpu_seconds,
          snapshot->wall_delta_seconds, snapshot->cpu_delta_seconds,
          snapshot->cpu_percent, snapshot->rss_kb, snapshot->hwm_kb,
          snapshot->vm_kb, snapshot->footprint_kb, snapshot->threads,
          snapshot->open_fds, snapshot->voluntary_ctxt,
          snapshot->involuntary_ctxt, snapshot->minor_faults,
          snapshot->major_faults, snapshot->pageins, snapshot->messages_sent,
          snapshot->messages_received, snapshot->block_inputs,
          snapshot->block_outputs, snapshot->faults_total, snapshot->cow_faults,
          snapshot->unix_syscalls, snapshot->mach_syscalls,
          snapshot->task_csw_total, (int)snapshot->pid, (int)snapshot->ppid,
          snapshot->interactive_count, snapshot->ui_count, snapshot->bg_count,
          snapshot->interactive_budget, snapshot->ui_budget,
          snapshot->bg_budget, snapshot->scheduler_steps, snapshot->work_units);
  fflush(g_metrics_file);
}

static void update_summary(const ProcessSnapshot *snapshot) {
  int queue_depth =
      snapshot->interactive_count + snapshot->ui_count + snapshot->bg_count;

  summary_state.sample_count++;
  summary_state.cpu_percent_sum += snapshot->cpu_percent;

  if (summary_state.sample_count == 1 ||
      snapshot->cpu_percent > summary_state.peak_cpu_percent) {
    summary_state.peak_cpu_percent = snapshot->cpu_percent;
    strncpy(summary_state.peak_cpu_phase, snapshot->phase,
            sizeof(summary_state.peak_cpu_phase) - 1);
    summary_state.peak_cpu_phase[sizeof(summary_state.peak_cpu_phase) - 1] =
        '\0';
  }
  if (snapshot->rss_kb > summary_state.peak_rss_kb) {
    summary_state.peak_rss_kb = snapshot->rss_kb;
  }
  if (snapshot->hwm_kb > summary_state.peak_hwm_kb) {
    summary_state.peak_hwm_kb = snapshot->hwm_kb;
  }
  if (snapshot->footprint_kb > summary_state.peak_footprint_kb) {
    summary_state.peak_footprint_kb = snapshot->footprint_kb;
  }
  if (snapshot->vm_kb > summary_state.peak_vm_kb) {
    summary_state.peak_vm_kb = snapshot->vm_kb;
  }
  if (snapshot->threads > summary_state.peak_threads) {
    summary_state.peak_threads = snapshot->threads;
  }
  if (snapshot->open_fds > summary_state.peak_open_fds) {
    summary_state.peak_open_fds = snapshot->open_fds;
  }
  if (queue_depth > summary_state.peak_queue_depth) {
    summary_state.peak_queue_depth = queue_depth;
  }

  if (summary_state.has_previous) {
    summary_state.total_vcsw_delta += clamp_nonnegative_delta(
        snapshot->voluntary_ctxt, summary_state.previous.voluntary_ctxt);
    summary_state.total_ivcsw_delta += clamp_nonnegative_delta(
        snapshot->involuntary_ctxt, summary_state.previous.involuntary_ctxt);
    summary_state.total_minor_fault_delta += clamp_nonnegative_delta(
        snapshot->minor_faults, summary_state.previous.minor_faults);
    summary_state.total_major_fault_delta += clamp_nonnegative_delta(
        snapshot->major_faults, summary_state.previous.major_faults);
    summary_state.total_pageins_delta += clamp_nonnegative_delta(
        snapshot->pageins, summary_state.previous.pageins);
    summary_state.total_block_inputs_delta += clamp_nonnegative_delta(
        snapshot->block_inputs, summary_state.previous.block_inputs);
    summary_state.total_block_outputs_delta += clamp_nonnegative_delta(
        snapshot->block_outputs, summary_state.previous.block_outputs);
    summary_state.total_messages_sent_delta += clamp_nonnegative_delta(
        snapshot->messages_sent, summary_state.previous.messages_sent);
    summary_state.total_messages_received_delta += clamp_nonnegative_delta(
        snapshot->messages_received, summary_state.previous.messages_received);
  }

  summary_state.previous = *snapshot;
  summary_state.has_previous = true;
}

static void note_queue_depth(void) {
  if (active_feather != NULL) {
    int queue_depth = active_feather->scheduler.interactiveCount +
                      active_feather->scheduler.uiCount +
                      active_feather->scheduler.bgCount;

    if (queue_depth > summary_state.peak_queue_depth) {
      summary_state.peak_queue_depth = queue_depth;
    }
  }
}

static void write_summary(void) {
  double avg_cpu_percent = 0.0;

  if (summary_state.sample_count > 0) {
    avg_cpu_percent =
        summary_state.cpu_percent_sum / (double)summary_state.sample_count;
  }

  log_puts("");
  log_puts("[summary] resource analysis");
  log_printf(
      "[summary] samples=%llu | avg_cpu=%.2f%% | peak_cpu=%.2f%% at %s\n",
      summary_state.sample_count, avg_cpu_percent,
      summary_state.peak_cpu_percent,
      summary_state.peak_cpu_phase[0] ? summary_state.peak_cpu_phase : "n/a");
  log_printf("[summary] peak_rss=%ld KB | peak_hwm=%ld KB | peak_footprint=%ld "
             "KB | peak_vm=%ld KB\n",
             summary_state.peak_rss_kb, summary_state.peak_hwm_kb,
             summary_state.peak_footprint_kb, summary_state.peak_vm_kb);
  log_printf(
      "[summary] peak_threads=%ld | peak_open_fds=%ld | peak_queue_depth=%d\n",
      summary_state.peak_threads, summary_state.peak_open_fds,
      summary_state.peak_queue_depth);
  log_printf(
      "[summary] vcsw_delta=%lld | ivcsw_delta=%lld | minor_fault_delta=%lld | "
      "major_fault_delta=%lld | pageins_delta=%lld\n",
      summary_state.total_vcsw_delta, summary_state.total_ivcsw_delta,
      summary_state.total_minor_fault_delta,
      summary_state.total_major_fault_delta, summary_state.total_pageins_delta);
  log_printf("[summary] block_in_delta=%lld | block_out_delta=%lld | "
             "msg_sent_delta=%lld | msg_recv_delta=%lld\n",
             summary_state.total_block_inputs_delta,
             summary_state.total_block_outputs_delta,
             summary_state.total_messages_sent_delta,
             summary_state.total_messages_received_delta);
  log_printf("[summary] scheduler_steps=%lld | work_units=%llu\n",
             runtime_state.scheduler_steps, runtime_state.work_units);
  log_printf("[summary] workload sum=%lld | sum_squares=%lld | weighted=%lld | "
             "popcount=%lld | checksum=%llu\n",
             fast_workload.sum, fast_workload.sum_squares,
             fast_workload.weighted_sum, fast_workload.popcount_total,
             (unsigned long long)fast_workload.checksum);
}

static void redraw_live_status_line(const ProcessSnapshot *snapshot) {
  char line[256];
  size_t offset = 0;
  size_t columns;

  if (!g_stdout_is_tty) {
    return;
  }

  line[0] = '\0';
  append_line_fragment(line, sizeof(line), &offset,
                       "[fast] %-9s wall=%4.2fs cpu=%5.1f%%", snapshot->phase,
                       snapshot->wall_seconds, snapshot->cpu_percent);

  if (snapshot->rss_kb >= 0) {
    append_line_fragment(line, sizeof(line), &offset, " rss=%ldK",
                         snapshot->rss_kb);
  }
  if (snapshot->footprint_kb >= 0) {
    append_line_fragment(line, sizeof(line), &offset, " foot=%ldK",
                         snapshot->footprint_kb);
  }
  if (snapshot->threads >= 0) {
    append_line_fragment(line, sizeof(line), &offset, " thr=%ld",
                         snapshot->threads);
  }
  if (snapshot->open_fds >= 0) {
    append_line_fragment(line, sizeof(line), &offset, " fd=%ld",
                         snapshot->open_fds);
  }
  if (snapshot->minor_faults >= 0) {
    append_line_fragment(line, sizeof(line), &offset, " minflt=%ld",
                         snapshot->minor_faults);
  }
  if (snapshot->involuntary_ctxt >= 0) {
    append_line_fragment(line, sizeof(line), &offset, " ivcsw=%ld",
                         snapshot->involuntary_ctxt);
  }

  columns = stdout_columns();
  if (columns > 1 && strlen(line) >= columns) {
    line[columns - 1] = '\0';
  }

  fputs("\r\033[2K", stdout);
  fputs(line, stdout);
  fflush(stdout);
  g_live_line_visible = 1;
}

static void record_snapshot(const char *phase) {
  ProcessSnapshot snapshot;

  capture_process_snapshot(&snapshot, phase);
  update_summary(&snapshot);
  write_metrics_row(&snapshot);
  redraw_live_status_line(&snapshot);
  runtime_state.last_emit_wall = runtime_state.last_wall_sample;
}

static void maybe_record_snapshot(const char *phase) {
  double now = monotonic_seconds();

  if (now - runtime_state.last_emit_wall >= FAST_SAMPLE_INTERVAL_SEC) {
    record_snapshot(phase);
  }
}

static bool feather_has_pending_tasks(const struct Feather *feather) {
  return Feather_has_pending_tasks(feather);
}

static bool schedule_task(void (*task_fn)(void *context), int8_t priority,
                          const char *label) {
  FSSchedulerInstantTask task = {.task = task_fn, .priority = priority};

  if (!Feather_add_instant_task(active_feather, task)) {
    demo_failed = true;
    error_printf("[error] failed to schedule \"%s\" on priority %d\n", label,
                 priority);
    return false;
  }

  note_queue_depth();
  log_printf("[schedule] %s -> priority=%d\n", label, priority);
  return true;
}

static int popcount_u32(uint32_t value) {
  int count = 0;

  while (value != 0U) {
    count += (int)(value & 1U);
    value >>= 1U;
  }
  return count;
}

static uint64_t checksum_step(uint64_t checksum, uint64_t contribution) {
  return (checksum * 911382323ULL + contribution) % FAST_CHECKSUM_MOD;
}

static void reset_fast_workload(void) {
  memset(&fast_workload, 0, sizeof(fast_workload));
  fast_workload.start_value = FAST_START_VALUE;
  fast_workload.end_value = FAST_END_VALUE;
  fast_workload.next_value = FAST_START_VALUE;
  fast_workload.chunk_size = FAST_CHUNK_SIZE;
  fast_workload.total_chunks =
      (FAST_END_VALUE - FAST_START_VALUE + FAST_CHUNK_SIZE) / FAST_CHUNK_SIZE;
  fast_workload.checksum = 1ULL;
}

static void fast_done_task(void *context) {
  (void)context;
  fast_workload.completed = true;
  log_puts("[interactive] fast pipeline complete");
}

static void fast_report_task(void *context) {
  (void)context;
  log_printf("[ui] chunks=%d/%d | sum=%lld | squares=%lld | weighted=%lld | "
             "popcount=%lld | checksum=%llu\n",
             fast_workload.chunks_done, fast_workload.total_chunks,
             fast_workload.sum, fast_workload.sum_squares,
             fast_workload.weighted_sum, fast_workload.popcount_total,
             (unsigned long long)fast_workload.checksum);
  schedule_task(fast_done_task, FSScheduler_Priority_INTERACTIVE, "fast done");
}

static void fast_chunk_task(void *context) {
  (void)context;
  int start = fast_workload.next_value;
  int end = start + fast_workload.chunk_size - 1;
  int value;

  if (end > fast_workload.end_value) {
    end = fast_workload.end_value;
  }

  for (value = start; value <= end; value++) {
    long long x = value;
    long long square = x * x;
    long long weight =
        square + (17LL * x) + (long long)popcount_u32((uint32_t)value);

    fast_workload.sum += x;
    fast_workload.sum_squares += square;
    fast_workload.weighted_sum += weight;
    fast_workload.popcount_total += popcount_u32((uint32_t)value);
    fast_workload.even_count += (value % 2 == 0) ? 1 : 0;
    fast_workload.odd_count += (value % 2 != 0) ? 1 : 0;
    fast_workload.checksum =
        checksum_step(fast_workload.checksum, (uint64_t)(weight + square + x));
    runtime_state.work_units++;
  }

  fast_workload.next_value = end + 1;
  fast_workload.chunks_done++;

  log_printf(
      "[background] chunk=%d/%d values=%d..%d | sum=%lld | checksum=%llu\n",
      fast_workload.chunks_done, fast_workload.total_chunks, start, end,
      fast_workload.sum, (unsigned long long)fast_workload.checksum);

  if (fast_workload.next_value <= fast_workload.end_value) {
    schedule_task(fast_chunk_task, FSScheduler_Priority_BACKGROUND,
                  "next fast chunk");
  } else {
    schedule_task(fast_report_task, FSScheduler_Priority_UI, "fast report");
  }
}

static void drain_scheduler(struct Feather *feather) {
  while (runtime_state.scheduler_steps < FAST_SAFETY_MAX_STEPS) {
    bool stepped;

    maybe_record_snapshot("scheduler");
    stepped = Feather_step(feather);
    if (stepped) {
      runtime_state.scheduler_steps++;
      continue;
    }

    if (!feather_has_pending_tasks(feather)) {
      break;
    }

    {
      struct timespec ts;

      ts.tv_sec = 0;
      ts.tv_nsec = FAST_IDLE_SLEEP_NSEC;
      nanosleep(&ts, NULL);
    }
  }

  if (runtime_state.scheduler_steps >= FAST_SAFETY_MAX_STEPS &&
      feather_has_pending_tasks(feather)) {
    demo_failed = true;
    error_printf("[error] scheduler hit safety limit\n");
  }
}

static void run_fast_pipeline(void) {
  struct Feather feather;

  if (!Feather_init(&feather)) {
    demo_failed = true;
    error_printf("[error] Feather_init failed\n");
    return;
  }

  log_puts("[pipeline] start compact workload");
  active_feather = &feather;
  reset_fast_workload();

  schedule_task(fast_chunk_task, FSScheduler_Priority_BACKGROUND,
                "first fast chunk");
  record_snapshot("queued");
  drain_scheduler(&feather);
  record_snapshot("post-sched");

  Feather_deinit(&feather);
  active_feather = NULL;
}

static void perform_profile_burst(double target_seconds) {
  uint64_t state = 0xA0761D6478BD642FULL;
  uint64_t accumulator = 0;
  unsigned long long profile_iterations = 0;
  double start = monotonic_seconds();

  log_puts("[profile] start short cpu burst");

  while ((monotonic_seconds() - start) < target_seconds) {
    int i;

    for (i = 0; i < 50000; i++) {
      state ^= state >> 12;
      state ^= state << 25;
      state ^= state >> 27;
      state *= 2685821657736338717ULL;
      accumulator ^= state + (uint64_t)i;
    }

    runtime_state.work_units += 50000ULL;
    profile_iterations += 50000ULL;
    maybe_record_snapshot("profile");
  }

  log_printf("[profile] done | profile_iterations=%llu | total_work_units=%llu "
             "| accumulator=%llu\n",
             profile_iterations, runtime_state.work_units,
             (unsigned long long)accumulator);
}

int main(void) {
  memset(&runtime_state, 0, sizeof(runtime_state));
  memset(&summary_state, 0, sizeof(summary_state));
  g_stdout_is_tty = isatty(STDOUT_FILENO) != 0;

  runtime_state.wall_start = monotonic_seconds();
  runtime_state.cpu_start = process_cpu_seconds();
  runtime_state.last_emit_wall = runtime_state.wall_start;
  runtime_state.last_wall_sample = runtime_state.wall_start;
  runtime_state.last_cpu_sample = runtime_state.cpu_start;

  initialize_logs();
  record_snapshot("startup");
  run_fast_pipeline();

  if (!demo_failed && !runtime_state.stop_requested) {
    perform_profile_burst(FAST_PROFILE_SECONDS);
  }

  record_snapshot("shutdown");
  write_summary();

  if (g_metrics_file != NULL) {
    fclose(g_metrics_file);
    g_metrics_file = NULL;
  }
  if (g_log_file != NULL) {
    fclose(g_log_file);
    g_log_file = NULL;
  }

  finish_live_line();

  if (demo_failed || !fast_workload.completed) {
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
