#ifndef FEATHER_HPP
#define FEATHER_HPP

#include "FeatherC.hpp"

#ifdef __cplusplus

#include "FeatherRuntime/FSResourceTracker.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <new>
#include <type_traits>
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

struct InstantTask {
  MoveOnlyFunction<void()> callable;
  Priority priority = Priority::Background;
  std::uint64_t deadline_ms = 0;
  std::uint64_t timeout_ms = 0;
};

struct DeferredTask {
  MoveOnlyFunction<void()> callable;
  Priority priority = Priority::Background;
  std::uint64_t start_time_ms = 0;
  std::uint64_t deadline_ms = 0;
  std::uint64_t timeout_ms = 0;
};

struct RepeatingTask {
  MoveOnlyFunction<void()> callable;
  Priority priority = Priority::Background;
  std::uint64_t start_time_ms = 0;
  std::uint64_t repeat_interval_ms = 0; /* required (> 0) for repeating tasks */
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
  TaskHandle schedule(InstantTask task);
  TaskHandle schedule(DeferredTask task);
  TaskHandle schedule(RepeatingTask task);
  bool cancel(TaskHandle handle);
  bool set_time_source(std::uint64_t (*now_fn)(void *context), void *context);
  bool set_time_provider(const ::FSTime *provider);
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

namespace FSTime {
using Provider = ::FSTime;

inline const Provider *default_provider() { return &::FSTime_init; }
inline std::uint64_t now_monotonic_ms() { return ::FSTime_now_monotonic(); }
inline std::uint64_t now_unix_ms() { return ::FSTime_now_unix(); }
inline bool sleep_ms(std::uint64_t duration_ms) {
  return ::FSTime_sleep_ms(duration_ms);
}
} // namespace FSTime

namespace FSAllocator {
using Allocator = ::FSAllocator;

class API {
public:
  static const Allocator *system() { return &::FSAllocator_system; }
  static const Allocator *resolve(const Allocator *allocator) {
    return ::FSAllocator_resolve(allocator);
  }
  static void *allocate(const Allocator *allocator, std::size_t size) {
    return ::FSAllocator_allocate(allocator, size);
  }
  static void *reallocate(const Allocator *allocator, void *pointer,
                          std::size_t size) {
    return ::FSAllocator_reallocate(allocator, pointer, size);
  }
  static void deallocate(const Allocator *allocator, void *pointer) {
    ::FSAllocator_deallocate(allocator, pointer);
  }
};

inline const Allocator *system() { return API::system(); }
inline const Allocator *resolve(const Allocator *allocator) {
  return API::resolve(allocator);
}
inline void *allocate(const Allocator *allocator, std::size_t size) {
  return API::allocate(allocator, size);
}
inline void *reallocate(const Allocator *allocator, void *pointer,
                        std::size_t size) {
  return API::reallocate(allocator, pointer, size);
}
inline void deallocate(const Allocator *allocator, void *pointer) {
  API::deallocate(allocator, pointer);
}

template <typename T>
struct TypedDeleter {
  const Allocator *allocator = resolve(nullptr);

  void operator()(T *pointer) const noexcept {
    if (pointer == nullptr) {
      return;
    }
    pointer->~T();
    deallocate(allocator, pointer);
  }
};

template <typename T>
using UniquePtr = std::unique_ptr<T, TypedDeleter<T>>;

template <typename T, typename... Args>
UniquePtr<T> make_unique(const Allocator *allocator, Args &&...args) {
  const Allocator *resolved = resolve(allocator);
  void *raw_memory = allocate(resolved, sizeof(T));
  if (raw_memory == nullptr) {
    return UniquePtr<T>(nullptr, TypedDeleter<T>{resolved});
  }

  try {
    T *object = new (raw_memory) T(std::forward<Args>(args)...);
    return UniquePtr<T>(object, TypedDeleter<T>{resolved});
  } catch (...) {
    deallocate(resolved, raw_memory);
    throw;
  }
}
} // namespace FSAllocator

namespace FSScheduler {
using RawScheduler = ::FSScheduler;
using InstantTask = ::FSSchedulerInstantTask;
using DeferredTask = ::FSSchedulerDeferredTask;
using RepeatingTask = ::FSSchedulerRepeatingTask;
using RepeatMode = ::FSSchedulerTaskRepeatMode;
using TaskStatus = ::FSSchedulerTaskStatus;
using TaskHandler = ::FSSchedulerTaskHandler;
using StateSnapshot = ::FSSchedulerStateSnapshot;
using ComponentMemorySnapshot = ::FSSchedulerComponentMemorySnapshot;

class API {
public:
  static std::uint64_t now_ms() { return ::FSScheduler_now_ms(); }
  static void init(RawScheduler *core) { ::FSScheduler_init(core); }
  static void init_with_allocator(RawScheduler *core,
                                  const FSAllocator::Allocator *allocator) {
    ::FSScheduler_init_with_allocator(core, allocator);
  }
  static void deinit(RawScheduler *core) { ::FSScheduler_deinit(core); }
  static std::uint64_t add_instant_task(RawScheduler *core, InstantTask task) {
    return ::FSScheduler_add_instant_task(core, task);
  }
  static std::uint64_t add_deferred_task(RawScheduler *core,
                                         DeferredTask task) {
    return ::FSScheduler_add_deferred_task(core, task);
  }
  static std::uint64_t add_repeating_task(RawScheduler *core,
                                          RepeatingTask task) {
    return ::FSScheduler_add_repeating_task(core, task);
  }
  static bool set_time_source(RawScheduler *core,
                              std::uint64_t (*now_fn)(void *context),
                              void *context) {
    return ::FSScheduler_set_time_source(core, now_fn, context);
  }
  static bool set_time_provider(RawScheduler *core, const ::FSTime *provider) {
    return ::FSScheduler_set_time_provider(core, provider);
  }
  static bool has_pending_tasks(const RawScheduler *core) {
    return ::FSScheduler_has_pending_tasks(core);
  }
  static bool next_sleep_ms(const RawScheduler *core,
                            std::uint64_t *out_delay_ms) {
    return ::FSScheduler_next_sleep_ms(core, out_delay_ms);
  }
  static bool process_for_ms(RawScheduler *core, std::uint64_t duration_ms) {
    return ::FSScheduler_process_for_ms(core, duration_ms);
  }
  static bool step(RawScheduler *core) { return ::FSScheduler_step(core); }
  static ComponentMemorySnapshot component_memory_snapshot(
      const RawScheduler *core) {
    return ::FSScheduler_component_memory_snapshot(core);
  }
  static bool cancel_task(RawScheduler *core, std::uint64_t task_id) {
    return ::FSScheduler_cancel_task(core, task_id);
  }
  static bool pause_task(RawScheduler *core, std::uint64_t task_id) {
    return ::FSScheduler_pause_task(core, task_id);
  }
  static bool resume_task(RawScheduler *core, std::uint64_t task_id) {
    return ::FSScheduler_resume_task(core, task_id);
  }
  static bool reschedule_task(RawScheduler *core, std::uint64_t task_id,
                              std::uint64_t start_time_ms) {
    return ::FSScheduler_reschedule_task(core, task_id, start_time_ms);
  }
  static bool set_task_deadline(RawScheduler *core, std::uint64_t task_id,
                                std::uint64_t deadline_ms) {
    return ::FSScheduler_set_task_deadline(core, task_id, deadline_ms);
  }
  static bool set_task_timeout(RawScheduler *core, std::uint64_t task_id,
                               std::uint64_t timeout_ms) {
    return ::FSScheduler_set_task_timeout(core, task_id, timeout_ms);
  }
  static TaskStatus task_status(const RawScheduler *core, std::uint64_t task_id) {
    return ::FSScheduler_task_status(core, task_id);
  }
  static StateSnapshot state_snapshot(const RawScheduler *core) {
    return ::FSScheduler_state_snapshot(core);
  }
};

inline std::uint64_t now_ms() { return API::now_ms(); }
inline void init(RawScheduler *core) { API::init(core); }
inline void init_with_allocator(RawScheduler *core,
                                const FSAllocator::Allocator *allocator) {
  API::init_with_allocator(core, allocator);
}
inline void deinit(RawScheduler *core) { API::deinit(core); }

class Instance {
public:
  Instance() { API::init(&scheduler_); }

  explicit Instance(const FSAllocator::Allocator *allocator) {
    API::init_with_allocator(&scheduler_, allocator);
  }

  ~Instance() { API::deinit(&scheduler_); }

  Instance(const Instance &) = delete;
  Instance &operator=(const Instance &) = delete;
  Instance(Instance &&) = delete;
  Instance &operator=(Instance &&) = delete;

  [[nodiscard]] RawScheduler *get() { return &scheduler_; }
  [[nodiscard]] const RawScheduler *get() const { return &scheduler_; }
  [[nodiscard]] std::uint64_t add_instant_task(InstantTask task) {
    return API::add_instant_task(&scheduler_, task);
  }
  [[nodiscard]] std::uint64_t add_deferred_task(DeferredTask task) {
    return API::add_deferred_task(&scheduler_, task);
  }
  [[nodiscard]] std::uint64_t add_repeating_task(RepeatingTask task) {
    return API::add_repeating_task(&scheduler_, task);
  }
  bool set_time_source(std::uint64_t (*now_fn)(void *context), void *context) {
    return API::set_time_source(&scheduler_, now_fn, context);
  }
  bool set_time_provider(const ::FSTime *provider) {
    return API::set_time_provider(&scheduler_, provider);
  }
  [[nodiscard]] bool has_pending_tasks() const {
    return API::has_pending_tasks(&scheduler_);
  }
  bool next_sleep_ms(std::uint64_t *out_delay_ms) const {
    return API::next_sleep_ms(&scheduler_, out_delay_ms);
  }
  bool process_for_ms(std::uint64_t duration_ms) {
    return API::process_for_ms(&scheduler_, duration_ms);
  }
  bool step() { return API::step(&scheduler_); }
  [[nodiscard]] ComponentMemorySnapshot component_memory_snapshot() const {
    return API::component_memory_snapshot(&scheduler_);
  }
  bool cancel_task(std::uint64_t task_id) {
    return API::cancel_task(&scheduler_, task_id);
  }
  bool pause_task(std::uint64_t task_id) {
    return API::pause_task(&scheduler_, task_id);
  }
  bool resume_task(std::uint64_t task_id) {
    return API::resume_task(&scheduler_, task_id);
  }
  bool reschedule_task(std::uint64_t task_id, std::uint64_t start_time_ms) {
    return API::reschedule_task(&scheduler_, task_id, start_time_ms);
  }
  bool set_task_deadline(std::uint64_t task_id, std::uint64_t deadline_ms) {
    return API::set_task_deadline(&scheduler_, task_id, deadline_ms);
  }
  bool set_task_timeout(std::uint64_t task_id, std::uint64_t timeout_ms) {
    return API::set_task_timeout(&scheduler_, task_id, timeout_ms);
  }
  [[nodiscard]] TaskStatus task_status(std::uint64_t task_id) const {
    return API::task_status(&scheduler_, task_id);
  }
  [[nodiscard]] StateSnapshot state_snapshot() const {
    return API::state_snapshot(&scheduler_);
  }

private:
  RawScheduler scheduler_{};
};
} // namespace FSScheduler

namespace FSResourceTracker {
using Tracker = ::FSResourceTracker;
using Config = ::FSResourceTrackerConfig;
using Record = ::FSResourceTrackerRecord;
using Snapshot = ::FSResourceTrackerSnapshot;
using SchedulerSnapshot = ::FSResourceTrackerSchedulerSnapshot;

inline const Config *default_config() { return &::FSResourceTrackerConfig_init; }
inline bool init(Tracker *tracker) { return ::FSResourceTracker_init(tracker); }
inline bool init_with_config(Tracker *tracker, const Config *config) {
  return ::FSResourceTracker_init_with_config(tracker, config);
}
inline void deinit(Tracker *tracker) { ::FSResourceTracker_deinit(tracker); }
inline const FSAllocator::Allocator *allocator(const Tracker *tracker) {
  return ::FSResourceTracker_allocator(tracker);
}
inline Snapshot snapshot(const Tracker *tracker) {
  return ::FSResourceTracker_snapshot(tracker);
}
inline std::size_t copy_active_records(const Tracker *tracker, Record *out_records,
                                       std::size_t max_records) {
  return ::FSResourceTracker_copy_active_records(tracker, out_records,
                                                 max_records);
}
inline bool has_leaks(const Tracker *tracker) {
  return ::FSResourceTracker_has_leaks(tracker);
}
inline SchedulerSnapshot scheduler_snapshot(const Tracker *tracker,
                                            const FSScheduler::RawScheduler *scheduler_ref) {
  return ::FSResourceTracker_scheduler_snapshot(tracker, scheduler_ref);
}

class Instance {
public:
  Instance() : initialized_(init(&tracker_)) {}

  explicit Instance(const Config &config)
      : initialized_(init_with_config(&tracker_, &config)) {}

  ~Instance() {
    if (initialized_) {
      deinit(&tracker_);
    }
  }

  Instance(const Instance &) = delete;
  Instance &operator=(const Instance &) = delete;
  Instance(Instance &&) = delete;
  Instance &operator=(Instance &&) = delete;

  [[nodiscard]] bool initialized() const { return initialized_; }
  [[nodiscard]] Tracker *get() { return &tracker_; }
  [[nodiscard]] const Tracker *get() const { return &tracker_; }

private:
  Tracker tracker_{};
  bool initialized_ = false;
};
} // namespace FSResourceTracker

} // namespace feather

#endif

#endif
