#include "Feather.hpp"

#include <cstdio>
#include <memory>

namespace {

int g_failures = 0;

void expect_true(bool condition, const char *message) {
  if (!condition) {
    std::fprintf(stderr, "[FAIL] %s\n", message);
    ++g_failures;
    return;
  }
  std::printf("[PASS] %s\n", message);
}

} // namespace

int main() {
  feather::Scheduler scheduler;
  expect_true(scheduler.initialized(), "scheduler initializes");
  expect_true(scheduler.set_time_source(
                  +[](void *) -> std::uint64_t { return 42; }, nullptr),
              "set custom time source through class API");
  expect_true(scheduler.set_time_provider(&FSTime_init),
              "set custom time provider through class API");
  const FSSchedulerStateSnapshot snapshot = scheduler.state_snapshot();
  expect_true(snapshot.total_pending == 0,
              "empty scheduler snapshot reports no pending tasks");
  expect_true(feather::FSTime::default_provider() != nullptr,
              "time namespace exposes default provider");
  expect_true(feather::FSAllocator::resolve(nullptr) == &FSAllocator_system,
              "allocator namespace resolves to system allocator");
  feather::FSResourceTracker::Config tracker_config{};
  expect_true(tracker_config.base_allocator == nullptr &&
                  tracker_config.now_fn == nullptr &&
                  tracker_config.now_context == nullptr,
              "resource_tracker namespace exposes usable config type");
  feather::FSScheduler::RawScheduler raw_scheduler{};
  feather::FSScheduler::init(&raw_scheduler);
  const std::uint64_t raw_now_ms = feather::FSScheduler::now_ms();
  (void)raw_now_ms;
  expect_true(true, "scheduler namespace exposes now_ms");
  feather::FSScheduler::deinit(&raw_scheduler);

  int counter = 0;
  auto captured_value = std::make_unique<int>(7);

  feather::TaskHandle run_once_handle =
      scheduler.schedule([value = std::move(captured_value), &counter]() mutable {
        counter += *value;
        value.reset();
      });

  expect_true(run_once_handle.valid(), "move-only callable can be scheduled");
  expect_true(scheduler.step(), "step executes scheduled move-only task");
  expect_true(counter == 7, "scheduled task updates counter");
  expect_true(scheduler.state(run_once_handle) == feather::TaskState::NotFound,
              "finished one-shot task is removed from scheduler");

  feather::TaskHandle cancelled_handle =
      scheduler.schedule([&counter]() { counter += 100; });
  expect_true(cancelled_handle.valid(), "second task scheduled");
  expect_true(scheduler.cancel(cancelled_handle), "cancel scheduled task");
  expect_true(!scheduler.step(), "step returns false after cancelled task");
  expect_true(counter == 7, "cancelled task does not execute");
  const auto memory_snapshot = scheduler.component_memory_snapshot();
  expect_true(memory_snapshot.total_bytes > 0,
              "component memory snapshot reports tracked allocator bytes");

  /* ---- Typed lambda-based task structs ---- */

  // InstantTask: schedule without prior variable declaration
  int instant_count = 0;
  feather::TaskHandle instant_handle = scheduler.schedule(feather::InstantTask{
      .callable = [&instant_count]() { instant_count += 1; },
      .priority = feather::Priority::Background,
  });
  expect_true(instant_handle.valid(), "InstantTask: handle is valid");
  expect_true(scheduler.step(), "InstantTask: step executes task");
  expect_true(instant_count == 1, "InstantTask: callable was invoked");
  expect_true(
      scheduler.state(instant_handle) == feather::TaskState::NotFound,
      "InstantTask: finished task is removed from scheduler");

  // InstantTask with null callable returns invalid handle
  feather::TaskHandle null_instant_handle =
      scheduler.schedule(feather::InstantTask{});
  expect_true(!null_instant_handle.valid(),
              "InstantTask: null callable returns invalid handle");

  // DeferredTask: executes after start_time
  scheduler.set_time_source(+[](void *) -> std::uint64_t { return 500; },
                            nullptr);
  int deferred_count = 0;
  feather::TaskHandle deferred_handle = scheduler.schedule(feather::DeferredTask{
      .callable = [&deferred_count]() { deferred_count += 1; },
      .priority = feather::Priority::Background,
      .start_time_ms = 1000,
  });
  expect_true(deferred_handle.valid(), "DeferredTask: handle is valid");
  expect_true(!scheduler.step(),
              "DeferredTask: task does not run before start_time");
  expect_true(deferred_count == 0,
              "DeferredTask: callable not invoked before start_time");
  scheduler.set_time_source(+[](void *) -> std::uint64_t { return 1000; },
                            nullptr);
  expect_true(scheduler.step(),
              "DeferredTask: task runs at start_time");
  expect_true(deferred_count == 1, "DeferredTask: callable was invoked");

  // RepeatingTask: repeats on each cycle
  scheduler.set_time_provider(&FSTime_init);
  int repeat_count = 0;
  std::uint64_t fake_ms = 0;
  scheduler.set_time_source(
      +[](void *ctx) -> std::uint64_t {
        return *static_cast<std::uint64_t *>(ctx);
      },
      &fake_ms);
  feather::TaskHandle repeating_handle =
      scheduler.schedule(feather::RepeatingTask{
          .callable = [&repeat_count]() { repeat_count += 1; },
          .priority = feather::Priority::Background,
          .start_time_ms = 0,
          .repeat_interval_ms = 100,
          .repeat_mode = feather::RepeatMode::FixedDelay,
      });
  expect_true(repeating_handle.valid(), "RepeatingTask: handle is valid");
  expect_true(scheduler.step(), "RepeatingTask: first execution runs");
  expect_true(repeat_count == 1, "RepeatingTask: first execution counted");
  expect_true(!scheduler.step(),
              "RepeatingTask: task waits until next cycle");
  fake_ms = 150;
  expect_true(scheduler.step(),
              "RepeatingTask: second execution runs after interval");
  expect_true(repeat_count == 2, "RepeatingTask: second execution counted");
  expect_true(scheduler.cancel(repeating_handle),
              "RepeatingTask: can be cancelled");

  // RepeatingTask with zero interval returns invalid handle
  feather::TaskHandle zero_interval_handle =
      scheduler.schedule(feather::RepeatingTask{
          .callable = []() {},
          .repeat_interval_ms = 0,
      });
  expect_true(!zero_interval_handle.valid(),
              "RepeatingTask: zero interval returns invalid handle");

  if (g_failures == 0) {
    std::printf("[PASS] Feather C++ scheduler tests\n");
    return 0;
  }

  std::fprintf(stderr, "[FAIL] Feather C++ scheduler tests (%d failures)\n",
               g_failures);
  return 1;
}
