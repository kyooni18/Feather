#include "FeatherCpp.hpp"

#include <utility>

namespace feather {

Scheduler::Scheduler() : initialized_(Feather_init(&feather_)) {}

Scheduler::~Scheduler() {
  if (initialized_) {
    Feather_deinit(&feather_);
  }
}

TaskHandle Scheduler::schedule(Task task, const TaskOptions &options) {
  std::uint64_t task_id = 0;
  auto internal_task = std::make_unique<InternalTask>(std::move(task));

  if (!initialized_ || !internal_task->callable) {
    return {};
  }

  if (options.repeat_interval_ms != 0) {
    FSSchedulerRepeatingTask repeating_task = FSSchedulerRepeatingTask_init;
    repeating_task.task = &Scheduler::execute_task;
    repeating_task.context = internal_task.get();
    repeating_task.start_time = options.start_time_ms;
    repeating_task.execute_cycle = options.repeat_interval_ms;
    repeating_task.deadline = options.deadline_ms;
    repeating_task.timeout = options.timeout_ms;
    repeating_task.repeat_mode = to_repeat_mode(options.repeat_mode);
    repeating_task.priority = to_priority(options.priority);
    task_id = Feather_add_repeating_task(&feather_, repeating_task);
  } else if (options.start_time_ms != 0) {
    FSSchedulerDeferredTask deferred_task = FSSchedulerDeferredTask_init;
    deferred_task.task = &Scheduler::execute_task;
    deferred_task.context = internal_task.get();
    deferred_task.start_time = options.start_time_ms;
    deferred_task.deadline = options.deadline_ms;
    deferred_task.timeout = options.timeout_ms;
    deferred_task.priority = to_priority(options.priority);
    task_id = Feather_add_deferred_task(&feather_, deferred_task);
  } else {
    FSSchedulerInstantTask instant_task = FSSchedulerInstantTask_init;
    instant_task.task = &Scheduler::execute_task;
    instant_task.context = internal_task.get();
    instant_task.deadline = options.deadline_ms;
    instant_task.timeout = options.timeout_ms;
    instant_task.priority = to_priority(options.priority);
    task_id = Feather_add_instant_task(&feather_, instant_task);
  }

  if (task_id == 0) {
    return {};
  }

  tasks_[task_id] = std::move(internal_task);
  return TaskHandle(task_id);
}

bool Scheduler::cancel(TaskHandle handle) {
  if (!initialized_ || !handle.valid()) {
    return false;
  }

  if (!Feather_cancel_task(&feather_, handle.id())) {
    return false;
  }

  tasks_.erase(handle.id());
  return true;
}

bool Scheduler::set_time_source(std::uint64_t (*now_fn)(void *context),
                                void *context) {
  if (!initialized_) {
    return false;
  }
  return Feather_set_time_source(&feather_, now_fn, context);
}

bool Scheduler::set_time_provider(const FSTime *provider) {
  if (!initialized_) {
    return false;
  }
  return Feather_set_time_provider(&feather_, provider);
}

bool Scheduler::pause(TaskHandle handle) {
  if (!initialized_ || !handle.valid()) {
    return false;
  }
  return FSScheduler_pause_task(&feather_.scheduler, handle.id());
}

bool Scheduler::resume(TaskHandle handle) {
  if (!initialized_ || !handle.valid()) {
    return false;
  }
  return FSScheduler_resume_task(&feather_.scheduler, handle.id());
}

bool Scheduler::reschedule(TaskHandle handle, std::uint64_t start_time_ms) {
  if (!initialized_ || !handle.valid()) {
    return false;
  }
  return FSScheduler_reschedule_task(&feather_.scheduler, handle.id(),
                                     start_time_ms);
}

bool Scheduler::set_deadline(TaskHandle handle, std::uint64_t deadline_ms) {
  if (!initialized_ || !handle.valid()) {
    return false;
  }
  return FSScheduler_set_task_deadline(&feather_.scheduler, handle.id(),
                                       deadline_ms);
}

bool Scheduler::set_timeout(TaskHandle handle, std::uint64_t timeout_ms) {
  if (!initialized_ || !handle.valid()) {
    return false;
  }
  return FSScheduler_set_task_timeout(&feather_.scheduler, handle.id(),
                                      timeout_ms);
}

TaskState Scheduler::state(TaskHandle handle) const {
  if (!initialized_ || !handle.valid()) {
    return TaskState::NotFound;
  }
  return to_state(Feather_task_status(&feather_, handle.id()));
}

FeatherComponentMemorySnapshot Scheduler::component_memory_snapshot() const {
  if (!initialized_) {
    return FeatherComponentMemorySnapshot{};
  }
  return Feather_component_memory_snapshot(&feather_);
}

FSSchedulerStateSnapshot Scheduler::state_snapshot() const {
  if (!initialized_) {
    return FSSchedulerStateSnapshot{};
  }
  return Feather_state_snapshot(&feather_);
}

bool Scheduler::step() {
  if (!initialized_) {
    return false;
  }

  const bool step_result = Feather_step(&feather_);
  prune_finished_tasks();
  return step_result;
}

bool Scheduler::has_pending_tasks() const {
  return initialized_ && Feather_has_pending_tasks(&feather_);
}

bool Scheduler::next_sleep_ms(std::uint64_t &out_delay_ms) const {
  return initialized_ && Feather_next_sleep_ms(&feather_, &out_delay_ms);
}

bool Scheduler::process_for_ms(std::uint64_t duration_ms) {
  if (!initialized_) {
    return false;
  }
  const bool process_result = Feather_process_for_ms(&feather_, duration_ms);
  prune_finished_tasks();
  return process_result;
}

void Scheduler::execute_task(void *context) {
  if (context == nullptr) {
    return;
  }

  auto *task = static_cast<InternalTask *>(context);
  if (task->callable) {
    task->callable();
  }
}

std::uint8_t Scheduler::to_priority(Priority priority) {
  return static_cast<std::uint8_t>(priority);
}

FSSchedulerTaskRepeatMode Scheduler::to_repeat_mode(RepeatMode repeat_mode) {
  return static_cast<FSSchedulerTaskRepeatMode>(repeat_mode);
}

TaskState Scheduler::to_state(FSSchedulerTaskStatus status) {
  return static_cast<TaskState>(status);
}

void Scheduler::prune_finished_tasks() {
  for (auto it = tasks_.begin(); it != tasks_.end();) {
    const TaskState task_state = state(TaskHandle(it->first));
    if (task_state == TaskState::NotFound || task_state == TaskState::TimedOut) {
      it = tasks_.erase(it);
      continue;
    }
    ++it;
  }
}

} // namespace feather
