#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>

#ifdef __APPLE__
#include <mach/mach.h>
#endif

#include "Feather.hpp"
#include "FeatherRuntime/FSResourceTracker.hpp"

/* ---------------------------------------------------------------------------
 * Memory / resource snapshot
 * ------------------------------------------------------------------------- */

typedef struct {
    size_t resident_bytes;
    size_t virtual_bytes;
    long   user_us;
    long   sys_us;
} MemSnapshot;

static MemSnapshot take_snapshot(void)
{
    MemSnapshot s = {0, 0, 0, 0};

#ifdef __APPLE__
    struct mach_task_basic_info info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  (task_info_t)&info, &count) == KERN_SUCCESS) {
        s.resident_bytes = (size_t)info.resident_size;
        s.virtual_bytes  = (size_t)info.virtual_size;
    }
#endif

    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) == 0) {
        s.user_us = (long)ru.ru_utime.tv_sec * 1000000L + ru.ru_utime.tv_usec;
        s.sys_us  = (long)ru.ru_stime.tv_sec * 1000000L + ru.ru_stime.tv_usec;
    }
    return s;
}

static void print_snapshot(const char *label, const MemSnapshot *s)
{
    printf("  [%-30s]  RSS=%7.1f KB  VIRT=%9.1f KB"
           "  user=%6ld us  sys=%6ld us\n",
           label,
           (double)s->resident_bytes / 1024.0,
           (double)s->virtual_bytes  / 1024.0,
           s->user_us, s->sys_us);
}

static void print_delta(const char *label,
                        const MemSnapshot *before,
                        const MemSnapshot *after)
{
    long drss  = (long)after->resident_bytes - (long)before->resident_bytes;
    long dvirt = (long)after->virtual_bytes  - (long)before->virtual_bytes;
    long duser = after->user_us - before->user_us;
    long dsys  = after->sys_us  - before->sys_us;
    printf("  [%-30s]  ΔRSS=%+7ld B  ΔVIRT=%+9ld B"
           "  Δuser=%+6ld us  Δsys=%+6ld us\n",
           label, drss, dvirt, duser, dsys);
}

/* ---------------------------------------------------------------------------
 * Test helpers
 * ------------------------------------------------------------------------- */

static int       g_counter   = 0;
static uint64_t  g_fake_now  = 0;

typedef struct {
    Feather feather;
    FSResourceTracker tracking;
} TrackedFeather;

static void count_task(void *ctx) { (void)ctx; g_counter++; }

static uint64_t fake_now_fn(void *ctx) { (void)ctx; return g_fake_now; }

static bool check(bool cond, const char *msg)
{
    printf("%s: %s\n", cond ? "PASS" : "FAIL", msg);
    return cond;
}

static bool tracked_feather_init(TrackedFeather *tracked, bool use_fake_time)
{
    FeatherConfig config = FeatherConfig_init;

    if (tracked == NULL) {
        return false;
    }

    if (!FSResourceTracker_init(&tracked->tracking)) {
        return false;
    }

    config.allocator = FSResourceTracker_allocator(&tracked->tracking);
    config.now_fn = use_fake_time ? fake_now_fn : NULL;
    if (!Feather_init_with_config(&tracked->feather, &config)) {
        FSResourceTracker_deinit(&tracked->tracking);
        return false;
    }

    return true;
}

static bool tracked_feather_check_clean(const TrackedFeather *tracked,
                                        const char *message)
{
    FSResourceTrackerSnapshot snapshot;

    if (tracked == NULL) {
        return false;
    }

    snapshot = FSResourceTracker_snapshot(&tracked->tracking);
    return check(snapshot.active_allocations == 0 && snapshot.current_bytes == 0,
                 message);
}

static void tracked_feather_cleanup(TrackedFeather *tracked)
{
    if (tracked == NULL) {
        return;
    }

    FSResourceTracker_deinit(&tracked->tracking);
}

/* ---------------------------------------------------------------------------
 * Test 1 – init / deinit leaves no residual RSS
 * ------------------------------------------------------------------------- */

static bool test_init_deinit_leak(void)
{
    printf("\n=== Test 1: init/deinit baseline leak ===\n");
    MemSnapshot before = take_snapshot();
    TrackedFeather tracked;
    bool ok;

    ok = check(tracked_feather_init(&tracked, false),
               "init/deinit: tracked Feather initialized");
    if (!ok) {
        return false;
    }
    MemSnapshot mid = take_snapshot();
    print_delta("after init", &before, &mid);

    Feather_deinit(&tracked.feather);
    MemSnapshot after = take_snapshot();
    print_delta("after deinit", &before, &after);

    long leaked = (long)after.resident_bytes - (long)before.resident_bytes;
    /* Allow one 64 KB page of allocator bookkeeping slop. */
    ok = check(leaked < 65536, "init/deinit: no significant RSS growth") && ok;
    ok = tracked_feather_check_clean(&tracked,
                                     "init/deinit: tracker sees no active allocations after deinit") && ok;
    tracked_feather_cleanup(&tracked);
    return ok;
}

/* ---------------------------------------------------------------------------
 * Test 2 – waiting-heap growth is bounded and memory is fully freed
 * ------------------------------------------------------------------------- */

static bool test_waiting_heap_growth(void)
{
    printf("\n=== Test 2: waiting-heap growth & cleanup ===\n");
    bool ok = true;
    MemSnapshot before, peak, after;
    TrackedFeather tracked;

    before = take_snapshot();

    ok = check(tracked_feather_init(&tracked, true),
               "waiting-heap: tracked Feather initialized") && ok;
    if (!ok) {
        return false;
    }
    g_fake_now = 0;

    /* Push 512 future tasks into the waiting (min-heap) pool. */
    int accepted = 0;
    for (int i = 0; i < 512; i++) {
        FSSchedulerDeferredTask t = FSSchedulerDeferredTask_init;
        t.task        = count_task;
        t.priority    = FSScheduler_Priority_BACKGROUND;
        t.start_time  = (uint64_t)(100000 + i);
        if (Feather_add_deferred_task(&tracked.feather, t)) accepted++;
    }

    peak = take_snapshot();
    print_delta("peak (512 deferred tasks)", &before, &peak);
    printf("  Tasks accepted: %d / 512\n", accepted);

    ok = check(accepted == 512,
               "waiting-heap: all 512 deferred tasks accepted") && ok;

    /* Each FSSchedulerTaskRecord is 64 B; 512 tasks ≈ 32 KB.
     * Allow 512 KB for allocator overhead and OS page rounding. */
    long drss = (long)peak.resident_bytes - (long)before.resident_bytes;
    ok = check(drss < 512 * 1024,
               "waiting-heap: peak RSS growth < 512 KB") && ok;

    Feather_deinit(&tracked.feather);
    after = take_snapshot();
    print_delta("after deinit", &before, &after);

    ok = tracked_feather_check_clean(
             &tracked, "waiting-heap: tracker sees no active allocations after deinit") &&
         ok;
    tracked_feather_cleanup(&tracked);
    return ok;
}

/* ---------------------------------------------------------------------------
 * Test 3 – ready-queue stress across all three priorities
 * ------------------------------------------------------------------------- */

static bool test_ready_queue_stress(void)
{
    printf("\n=== Test 3: ready-queue stress (all priorities) ===\n");
    bool ok = true;
    MemSnapshot before, after;
    TrackedFeather tracked;

    g_counter  = 0;
    g_fake_now = 0;
    before = take_snapshot();

    ok = check(tracked_feather_init(&tracked, true),
               "ready-queue: tracked Feather initialized") && ok;
    if (!ok) {
        return false;
    }

    int added = 0;
    for (int i = 0; i < 300; i++) {
        FSSchedulerInstantTask t = FSSchedulerInstantTask_init;
        t.task     = count_task;
        t.priority = (i % 3 == 0 ? FSScheduler_Priority_INTERACTIVE
                     : i % 3 == 1 ? FSScheduler_Priority_UI
                                  : FSScheduler_Priority_BACKGROUND);
        if (Feather_add_instant_task(&tracked.feather, t)) added++;
    }

    int steps = 0;
    while (Feather_has_pending_tasks(&tracked.feather) && steps < 2000) {
        if (Feather_step(&tracked.feather)) steps++;
        else break;
    }

    after = take_snapshot();
    print_delta("after stress+drain", &before, &after);
    printf("  Added=%d  Steps=%d  Executed=%d\n", added, steps, g_counter);

    ok = check(g_counter == added,
               "ready-queue: all tasks executed") && ok;

    long leaked = (long)after.resident_bytes - (long)before.resident_bytes;
    ok = check(leaked < 65536,
               "ready-queue: no RSS growth after full drain") && ok;

    Feather_deinit(&tracked.feather);
    ok = tracked_feather_check_clean(
             &tracked, "ready-queue: tracker sees no active allocations after deinit") &&
         ok;
    tracked_feather_cleanup(&tracked);
    return ok;
}

/* ---------------------------------------------------------------------------
 * Test 4 – repeat task does not accumulate heap over many cycles
 * ------------------------------------------------------------------------- */

static bool test_repeat_task_no_heap_growth(void)
{
    printf("\n=== Test 4: repeat task heap stability ===\n");
    bool ok = true;
    MemSnapshot before, after;
    TrackedFeather tracked;

    g_counter  = 0;
    g_fake_now = 0;
    before = take_snapshot();

    ok = check(tracked_feather_init(&tracked, true),
               "repeat-task: tracked Feather initialized") && ok;
    if (!ok) {
        return false;
    }

    FSSchedulerRepeatingTask t = FSSchedulerRepeatingTask_init;
    t.task           = count_task;
    t.priority       = FSScheduler_Priority_UI;
    t.execute_cycle  = 10;
    t.repeat_mode    = FSSchedulerTaskRepeat_FIXEDRATE;
    Feather_add_repeating_task(&tracked.feather, t);

    for (int i = 0; i < 1000; i++) {
        g_fake_now = (uint64_t)(i * 10);
        Feather_step(&tracked.feather);
    }

    after = take_snapshot();
    print_delta("after 1000 repeat cycles", &before, &after);
    printf("  Executions: %d / 1000\n", g_counter);

    ok = check(g_counter >= 900,
               "repeat-task: ≥900 of 1000 cycles executed") && ok;

    long drss = (long)after.resident_bytes - (long)before.resident_bytes;
    ok = check(drss < 65536,
               "repeat-task: no RSS growth from cycling a single repeat task") && ok;

    Feather_deinit(&tracked.feather);
    ok = tracked_feather_check_clean(
             &tracked, "repeat-task: tracker sees no active allocations after deinit") &&
         ok;
    tracked_feather_cleanup(&tracked);
    return ok;
}

/* ---------------------------------------------------------------------------
 * Test 5 – TASK_CAPACITY hard limit is enforced, no overflow allocation
 * ------------------------------------------------------------------------- */

static bool test_capacity_limit(void)
{
    printf("\n=== Test 5: TASK_CAPACITY hard limit ===\n");
    bool ok = true;
    MemSnapshot before, after;
    TrackedFeather tracked;

    g_fake_now = 0;
    before = take_snapshot();

    ok = check(tracked_feather_init(&tracked, true),
               "capacity: tracked Feather initialized") && ok;
    if (!ok) {
        return false;
    }

    /* All tasks have future start_time so they land in the waiting heap. */
    int accepted = 0, rejected = 0;
    int try_count = FSScheduler_TASK_CAPACITY + 64;
    for (int i = 0; i < try_count; i++) {
        FSSchedulerDeferredTask t = FSSchedulerDeferredTask_init;
        t.task       = count_task;
        t.priority   = FSScheduler_Priority_BACKGROUND;
        t.start_time = (uint64_t)(1 + i);
        if (Feather_add_deferred_task(&tracked.feather, t)) accepted++;
        else rejected++;
    }

    after = take_snapshot();
    print_delta("at capacity", &before, &after);
    printf("  Accepted=%d  Rejected=%d  (TASK_CAPACITY=%d)\n",
           accepted, rejected, FSScheduler_TASK_CAPACITY);

    ok = check(accepted == FSScheduler_TASK_CAPACITY,
               "capacity: exactly TASK_CAPACITY tasks accepted") && ok;
    ok = check(rejected == 64,
               "capacity: overflow tasks rejected") && ok;

    /* RSS budget: capacity * sizeof(FSSchedulerTaskRecord) * 4 for allocator overhead. */
    long drss = (long)after.resident_bytes - (long)before.resident_bytes;
    long budget = (long)FSScheduler_TASK_CAPACITY * (long)sizeof(FSSchedulerTaskRecord) * 4;
    ok = check(drss < budget,
               "capacity: RSS growth within expected bounds") && ok;

    Feather_deinit(&tracked.feather);
    ok = tracked_feather_check_clean(
             &tracked, "capacity: tracker sees no active allocations after deinit") &&
         ok;
    tracked_feather_cleanup(&tracked);
    return ok;
}

/* ---------------------------------------------------------------------------
 * Test 6 – process_for_ms CPU-time is proportional (sanity check)
 * ------------------------------------------------------------------------- */

static bool test_process_for_ms_cpu(void)
{
    printf("\n=== Test 6: process_for_ms CPU usage ===\n");
    bool ok = true;
    TrackedFeather tracked;

    /* Load up many immediate tasks so process_for_ms has real work to do. */
    g_counter = 0;
    ok = check(tracked_feather_init(&tracked, false),
               "process_for_ms: tracked Feather initialized") && ok;
    if (!ok) {
        return false;
    }

    for (int i = 0; i < 200; i++) {
        FSSchedulerInstantTask t = FSSchedulerInstantTask_init;
        t.task     = count_task;
        t.priority = FSScheduler_Priority_UI;
        Feather_add_instant_task(&tracked.feather, t);
    }

    MemSnapshot before = take_snapshot();
    Feather_process_for_ms(&tracked.feather, 10);
    MemSnapshot after = take_snapshot();
    print_delta("process_for_ms(10 ms)", &before, &after);
    printf("  Tasks executed during window: %d\n", g_counter);

    ok = check(g_counter == 200,
               "process_for_ms: all 200 immediate tasks executed") && ok;

    Feather_deinit(&tracked.feather);
    ok = tracked_feather_check_clean(
             &tracked, "process_for_ms: tracker sees no active allocations after deinit") &&
         ok;
    tracked_feather_cleanup(&tracked);
    return ok;
}

/* ---------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */

int main(void)
{
    printf("Feather Memory & Resource Tracking Tests\n");
    printf("==========================================\n");

    MemSnapshot start = take_snapshot();
    print_snapshot("program start", &start);

    bool ok = true;
    ok = test_init_deinit_leak()          && ok;
    ok = test_waiting_heap_growth()       && ok;
    ok = test_ready_queue_stress()        && ok;
    ok = test_repeat_task_no_heap_growth() && ok;
    ok = test_capacity_limit()            && ok;
    ok = test_process_for_ms_cpu()        && ok;

    printf("\n=== Program totals ===\n");
    MemSnapshot end = take_snapshot();
    print_delta("overall", &start, &end);

    if (ok) {
        puts("\nAll memory/resource tests passed.");
        return EXIT_SUCCESS;
    }
    puts("\nSome memory/resource tests FAILED.");
    return EXIT_FAILURE;
}
