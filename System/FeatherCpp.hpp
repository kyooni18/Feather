#ifndef FEATHER_CPP_HPP
#define FEATHER_CPP_HPP

#include "Feather.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>

namespace feather {

enum class Priority : std::uint8_t {
  Background = FSScheduler_Priority_BACKGROUND,
  Ui = FSScheduler_Priority_UI,
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
  using Task = std::move_only_function<void()>;

  Scheduler();
  ~Scheduler();

  Scheduler(const Scheduler &) = delete;
  Scheduler &operator=(const Scheduler &) = delete;
  Scheduler(Scheduler &&) = delete;
  Scheduler &operator=(Scheduler &&) = delete;

  [[nodiscard]] bool initialized() const { return initialized_; }

  TaskHandle schedule(Task task, const TaskOptions &options = {});
  bool cancel(TaskHandle handle);
  bool pause(TaskHandle handle);
  bool resume(TaskHandle handle);
  bool reschedule(TaskHandle handle, std::uint64_t start_time_ms);
  bool set_deadline(TaskHandle handle, std::uint64_t deadline_ms);
  bool set_timeout(TaskHandle handle, std::uint64_t timeout_ms);
  [[nodiscard]] TaskState state(TaskHandle handle) const;

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
