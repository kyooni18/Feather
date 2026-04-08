#include <Feather.hpp>
#include <FeatherRuntime/FSResourceTracker.hpp>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#define TEST_DURATION_MS 10000ULL
#define INITIAL_SEED 0xC0FFEE42u
#define INITIAL_TASKS 256

typedef struct StressContext {
    Feather *feather;
    uint32_t rng;
    uint64_t start_ms;
    uint64_t end_ms;

    uint64_t executed_total;
    uint64_t executed_background;
    uint64_t executed_ui;
    uint64_t executed_interactive;

    uint64_t spawned_total;
    uint64_t spawned_instant;
    uint64_t spawned_deferred;
    uint64_t spawned_repeating;

    uint64_t loop_iterations;
    size_t peak_total_bytes;
} StressContext;

static uint64_t wall_now_ms(void) {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000ULL);
}

static uint32_t rng_next(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

static int rng_range(uint32_t *state, int min_value, int max_value) {
    uint32_t r = rng_next(state);
    int span = max_value - min_value + 1;
    return min_value + (int)(r % (uint32_t)span);
}

static uint8_t random_priority(uint32_t *state) {
    switch (rng_next(state) % 3u) {
        case 0: return FSScheduler_Priority_BACKGROUND;
        case 1: return FSScheduler_Priority_UI;
        default: return FSScheduler_Priority_INTERACTIVE;
    }
}

static void waste_cpu(uint32_t *state, int rounds) {
    volatile uint64_t acc = (uint64_t)(*state | 1u);
    for (int i = 0; i < rounds; ++i) {
        acc ^= acc << 7;
        acc ^= acc >> 9;
        acc *= UINT64_C(6364136223846793005);
        acc += UINT64_C(1442695040888963407);
    }
    *state ^= (uint32_t)(acc ^ (acc >> 32));
}

static void random_task(void *context);

static bool spawn_random_task(StressContext *ctx, int depth_bias) {
    uint64_t now = FSScheduler_now_ms();
    if (now >= ctx->end_ms) return false;

    uint32_t mode = rng_next(&ctx->rng) % 100u;
    bool ok = false;

    if (mode < 45u) {
        FSSchedulerInstantTask task = FSSchedulerInstantTask_init;
        task.task = random_task;
        task.context = ctx;
        task.priority = random_priority(&ctx->rng);
        ok = Feather_add_instant_task(ctx->feather, task);
        if (ok) ctx->spawned_instant++;
    } else if (mode < 80u) {
        FSSchedulerDeferredTask task = FSSchedulerDeferredTask_init;
        task.task = random_task;
        task.context = ctx;
        task.priority = random_priority(&ctx->rng);
        task.start_time = now + (uint64_t)rng_range(&ctx->rng, 0, 150);
        ok = Feather_add_deferred_task(ctx->feather, task);
        if (ok) ctx->spawned_deferred++;
    } else {
        FSSchedulerRepeatingTask task = FSSchedulerRepeatingTask_init;
        task.task = random_task;
        task.context = ctx;
        task.priority = random_priority(&ctx->rng);
        task.start_time = now + (uint64_t)rng_range(&ctx->rng, 0, 20);
        task.execute_cycle = (uint64_t)rng_range(&ctx->rng, 1, 30 + depth_bias);
        task.repeat_mode = (rng_next(&ctx->rng) & 1u)
            ? FSSchedulerTaskRepeat_FIXEDRATE
            : FSSchedulerTaskRepeat_FIXEDDELAY;
        ok = Feather_add_repeating_task(ctx->feather, task);
        if (ok) ctx->spawned_repeating++;
    }

    if (ok) ctx->spawned_total++;
    return ok;
}

static void random_task(void *context) {
    StressContext *ctx = (StressContext *)context;
    uint64_t now = FSScheduler_now_ms();
    if (now >= ctx->end_ms) return;

    ctx->executed_total++;

    switch (rng_next(&ctx->rng) % 3u) {
        case 0: ctx->executed_background++; break;
        case 1: ctx->executed_ui++; break;
        default: ctx->executed_interactive++; break;
    }

    waste_cpu(&ctx->rng, rng_range(&ctx->rng, 500, 6000));

    int spawn_count = rng_range(&ctx->rng, 0, 4);
    int depth_bias = rng_range(&ctx->rng, 0, 20);
    for (int i = 0; i < spawn_count; ++i) {
        spawn_random_task(ctx, depth_bias);
    }

    if ((rng_next(&ctx->rng) % 100u) < 8u) {
        waste_cpu(&ctx->rng, rng_range(&ctx->rng, 8000, 20000));
    }
}

int main(void) {
    FSResourceTracker tracker;
    Feather feather;
    FeatherConfig cfg = FeatherConfig_init;
    StressContext ctx = {0};

    if (!FSResourceTracker_init(&tracker)) return 1;

    cfg.allocator = FSResourceTracker_allocator(&tracker);
    if (!Feather_init_with_config(&feather, &cfg)) return 1;

    ctx.feather = &feather;
    ctx.rng = INITIAL_SEED;
    ctx.start_ms = FSScheduler_now_ms();
    ctx.end_ms = ctx.start_ms + TEST_DURATION_MS;
    ctx.peak_total_bytes = 0;

    for (int i = 0; i < INITIAL_TASKS; ++i) {
        spawn_random_task(&ctx, 0);
    }

    while (FSScheduler_now_ms() < ctx.end_ms) {
        Feather_step(&feather);
        ctx.loop_iterations++;

        FeatherComponentMemorySnapshot m =
            Feather_component_memory_snapshot(&feather);
        if (m.total_bytes > ctx.peak_total_bytes) {
            ctx.peak_total_bytes = m.total_bytes;
        }
    }

    FeatherComponentMemorySnapshot m =
        Feather_component_memory_snapshot(&feather);

    printf("Heavy Feather stress test finished\n");
    printf("Duration: ~%llu ms\n",
           (unsigned long long)(wall_now_ms() - ctx.start_ms));
    printf("Loop iterations: %llu\n",
           (unsigned long long)ctx.loop_iterations);

    printf("Executed total: %llu\n",
           (unsigned long long)ctx.executed_total);
    printf("Executed background-like: %llu\n",
           (unsigned long long)ctx.executed_background);
    printf("Executed UI-like: %llu\n",
           (unsigned long long)ctx.executed_ui);
    printf("Executed interactive-like: %llu\n",
           (unsigned long long)ctx.executed_interactive);

    printf("Spawned total: %llu\n",
           (unsigned long long)ctx.spawned_total);
    printf("Spawned instant: %llu\n",
           (unsigned long long)ctx.spawned_instant);
    printf("Spawned deferred: %llu\n",
           (unsigned long long)ctx.spawned_deferred);
    printf("Spawned repeating: %llu\n",
           (unsigned long long)ctx.spawned_repeating);

    printf("Current total memory used: %zu bytes\n", m.total_bytes);
    printf("Peak total memory used: %zu bytes\n", ctx.peak_total_bytes);

    return 0;
}