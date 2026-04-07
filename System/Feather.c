#include "Feather.h"

#include <stddef.h>
#include <string.h>

const FeatherConfig FeatherConfig_init = {
    .allocator = NULL, .now_fn = NULL, .now_context = NULL,
    .time_provider = NULL};

bool Feather_init(struct Feather *feather) {
  return Feather_init_with_config(feather, &FeatherConfig_init);
}

bool Feather_init_with_config(struct Feather *feather,
                               const FeatherConfig *config) {
  const FeatherConfig *resolved_config =
      (config != NULL) ? config : &FeatherConfig_init;

  if (feather == NULL) {
    return false;
  }

  FSScheduler_init_with_allocator(&feather->scheduler,
                                  resolved_config->allocator);
  if (resolved_config->time_provider != NULL &&
      !FSScheduler_set_time_provider(&feather->scheduler,
                                     resolved_config->time_provider)) {
    FSScheduler_deinit(&feather->scheduler);
    return false;
  }
  if (resolved_config->now_fn != NULL &&
      !FSScheduler_set_time_source(&feather->scheduler, resolved_config->now_fn,
                                   resolved_config->now_context)) {
    FSScheduler_deinit(&feather->scheduler);
    return false;
  }

  return true;
}

void Feather_deinit(struct Feather *feather) {
  if (feather == NULL) {
    return;
  }

  FSScheduler_deinit(&feather->scheduler);
}

uint64_t Feather_add_instant_task(struct Feather *feather,
                                  FSSchedulerInstantTask task) {
  if (feather == NULL || feather->scheduler.now_fn == NULL) {
    return 0;
  }

  return FSScheduler_add_instant_task(&feather->scheduler, task);
}

uint64_t Feather_add_deferred_task(struct Feather *feather,
                                   FSSchedulerDeferredTask task) {
  if (feather == NULL || feather->scheduler.now_fn == NULL) {
    return 0;
  }

  return FSScheduler_add_deferred_task(&feather->scheduler, task);
}

uint64_t Feather_add_repeating_task(struct Feather *feather,
                                    FSSchedulerRepeatingTask task) {
  if (feather == NULL || feather->scheduler.now_fn == NULL) {
    return 0;
  }

  return FSScheduler_add_repeating_task(&feather->scheduler, task);
}

bool Feather_set_time_source(struct Feather *feather,
                             uint64_t (*now_fn)(void *context),
                             void *context) {
  if (feather == NULL || feather->scheduler.now_fn == NULL) {
    return false;
  }

  return FSScheduler_set_time_source(&feather->scheduler, now_fn, context);
}

bool Feather_set_time_provider(struct Feather *feather,
                               const FSTime *provider) {
  if (feather == NULL || feather->scheduler.now_fn == NULL) {
    return false;
  }

  return FSScheduler_set_time_provider(&feather->scheduler, provider);
}

bool Feather_has_pending_tasks(const struct Feather *feather) {
  if (feather == NULL || feather->scheduler.now_fn == NULL) {
    return false;
  }

  return FSScheduler_has_pending_tasks(&feather->scheduler);
}

bool Feather_next_sleep_ms(const struct Feather *feather,
                           uint64_t *out_delay_ms) {
  if (feather == NULL || out_delay_ms == NULL ||
      feather->scheduler.now_fn == NULL) {
    return false;
  }

  return FSScheduler_next_sleep_ms(&feather->scheduler, out_delay_ms);
}

bool Feather_process_for_ms(struct Feather *feather, uint64_t duration_ms) {
  if (feather == NULL || feather->scheduler.now_fn == NULL) {
    return false;
  }

  return FSScheduler_process_for_ms(&feather->scheduler, duration_ms);
}

bool Feather_step(struct Feather *feather) {
  if (feather == NULL || feather->scheduler.now_fn == NULL) {
    return false;
  }

  return FSScheduler_step(&feather->scheduler);
}

FeatherComponentMemorySnapshot
Feather_component_memory_snapshot(const struct Feather *feather) {
  if (feather == NULL) {
    return (FeatherComponentMemorySnapshot){0, 0, 0, 0, 0};
  }

  return FSScheduler_component_memory_snapshot(&feather->scheduler);
}

bool Feather_cancel_task(struct Feather *feather, uint64_t task_id) {
  if (feather == NULL || feather->scheduler.now_fn == NULL) {
    return false;
  }

  return FSScheduler_cancel_task(&feather->scheduler, task_id);
}

FSSchedulerTaskStatus Feather_task_status(const struct Feather *feather,
                                          uint64_t task_id) {
  if (feather == NULL || feather->scheduler.now_fn == NULL) {
    return FSSchedulerTaskStatus_NOT_FOUND;
  }

  return FSScheduler_task_status(&feather->scheduler, task_id);
}

FSSchedulerStateSnapshot
Feather_state_snapshot(const struct Feather *feather) {
  FSSchedulerStateSnapshot empty;

  if (feather == NULL || feather->scheduler.now_fn == NULL) {
    memset(&empty, 0, sizeof(empty));
    return empty;
  }

  return FSScheduler_state_snapshot(&feather->scheduler);
}
