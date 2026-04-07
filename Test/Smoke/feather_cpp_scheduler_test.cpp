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

  std::printf("[PASS] Feather C++ scheduler tests\n");
  return (g_failures == 0) ? 0 : 1;
}
