#include "FeatherCpp.hpp"

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
  const FeatherComponentMemorySnapshot memory_snapshot =
      scheduler.component_memory_snapshot();
  expect_true(memory_snapshot.total_bytes > 0,
              "component memory snapshot reports tracked allocator bytes");

  if (g_failures == 0) {
    std::printf("[PASS] Feather C++ scheduler tests\n");
    return 0;
  }

  std::fprintf(stderr, "[FAIL] Feather C++ scheduler tests (%d failures)\n",
               g_failures);
  return 1;
}
