#ifndef FEATHER_CPP_HPP
#define FEATHER_CPP_HPP

#include "Feather.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>
#include <utility>

namespace feather {

#if __cplusplus >= 202302L
template <typename Sig>
using MoveOnlyFunction = std::move_only_function<Sig>;
#else
template <typename Sig>
class MoveOnlyFunction;

template <typename R, typename... Args>
class MoveOnlyFunction<R(Args...)> {
public:
  MoveOnlyFunction() = default;

  template <typename F,
            typename = std::enable_if_t<
                !std::is_same_v<std::decay_t<F>, MoveOnlyFunction> &&
                std::is_invocable_r_v<R, std::decay_t<F>, Args...>>>
  MoveOnlyFunction(F &&f)
      : impl_(std::make_unique<Impl<std::decay_t<F>>>(std::forward<F>(f))) {}

  MoveOnlyFunction(MoveOnlyFunction &&) = default;
  MoveOnlyFunction &operator=(MoveOnlyFunction &&) = default;
  MoveOnlyFunction(const MoveOnlyFunction &) = delete;
  MoveOnlyFunction &operator=(const MoveOnlyFunction &) = delete;

  explicit operator bool() const noexcept { return impl_ != nullptr; }

  R operator()(Args... args) {
    if (!impl_) {
      throw std::bad_function_call{};
    }
    return (*impl_)(std::forward<Args>(args)...);
  }

private:
  struct ImplBase {
    virtual R operator()(Args... args) = 0;
    virtual ~ImplBase() = default;
  };
  template <typename F>
  struct Impl final : ImplBase {
    explicit Impl(F &&f) : f_(std::move(f)) {}
    R operator()(Args... args) override {
      return f_(std::forward<Args>(args)...);
    }
    F f_;
  };
  std::unique_ptr<ImplBase> impl_;
};
#endif

enum class Priority : std::uint8_t {
  Background = FSScheduler_Priority_BACKGROUND,
  UI = FSScheduler_Priority_UI,
  Interactive = FSScheduler_Priority_INTERACTIVE
};

enum class RepeatMode : std::uint8_t {
  FixedDelay = FSSchedulerTaskRepeat_FIXEDDELAY,
  FixedRate = FSSchedulerTaskRepeat_FIXEDRATE
};

enum class TaskState : std::uint8_t {
  NotFound = FSSchedulerTaskStatus_NOT_FOUND,
  PendingReady = FSSchedulerTaskStatus_PENDING_READY,
  PendingWaiting = FSSchedulerTaskStatus_PENDING_WAITING,
  PendingPaused = FSSchedulerTaskStatus_PENDING_PAUSED,
  TimedOut = FSSchedulerTaskStatus_TIMED_OUT
};

struct TaskOptions {
  Priority priority = Priority::Background;
  std::uint64_t start_time_ms = 0;
  std::uint64_t repeat_interval_ms = 0;
  RepeatMode repeat_mode = RepeatMode::FixedDelay;
  std::uint64_t deadline_ms = 0;
  std::uint64_t timeout_ms = 0;
};

class TaskHandle {
public:
  TaskHandle() = default;

  [[nodiscard]] bool valid() const { return id_ != 0; }
  [[nodiscard]] std::uint64_t id() const { return id_; }

private:
  explicit TaskHandle(std::uint64_t id) : id_(id) {}

  std::uint64_t id_ = 0;

  friend class Scheduler;
};

class Scheduler {
public:
  using Task = MoveOnlyFunction<void()>;

  Scheduler();
  ~Scheduler();

  Scheduler(const Scheduler &) = delete;
  Scheduler &operator=(const Scheduler &) = delete;
  Scheduler(Scheduler &&) = delete;
  Scheduler &operator=(Scheduler &&) = delete;

  [[nodiscard]] bool initialized() const { return initialized_; }

  TaskHandle schedule(Task task, const TaskOptions &options = {});
  bool cancel(TaskHandle handle);
  bool set_time_source(std::uint64_t (*now_fn)(void *context), void *context);
  bool set_time_provider(const FSTime *provider);
  bool pause(TaskHandle handle);
  bool resume(TaskHandle handle);
  bool reschedule(TaskHandle handle, std::uint64_t start_time_ms);
  bool set_deadline(TaskHandle handle, std::uint64_t deadline_ms);
  bool set_timeout(TaskHandle handle, std::uint64_t timeout_ms);
  [[nodiscard]] TaskState state(TaskHandle handle) const;
  [[nodiscard]] FeatherComponentMemorySnapshot component_memory_snapshot() const;
  [[nodiscard]] FSSchedulerStateSnapshot state_snapshot() const;

  bool step();
  [[nodiscard]] bool has_pending_tasks() const;
  bool next_sleep_ms(std::uint64_t &out_delay_ms) const;
  bool process_for_ms(std::uint64_t duration_ms);

private:
  struct InternalTask {
    explicit InternalTask(Task &&callable_in)
        : callable(std::move(callable_in)) {}

    Task callable;
  };

  static void execute_task(void *context);
  static std::uint8_t to_priority(Priority priority);
  static FSSchedulerTaskRepeatMode to_repeat_mode(RepeatMode repeat_mode);
  static TaskState to_state(FSSchedulerTaskStatus status);
  void prune_finished_tasks();

  Feather feather_{};
  bool initialized_ = false;
  std::unordered_map<std::uint64_t, std::unique_ptr<InternalTask>> tasks_{};
};

} // namespace feather

#endif
