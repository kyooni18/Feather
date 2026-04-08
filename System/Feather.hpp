#ifndef FEATHER_H
#define FEATHER_H

#include "FeatherExport.hpp"
#include "FeatherRuntime/FSScheduler.hpp"

FEATHER_EXTERN_C_BEGIN

typedef struct FeatherConfig {
  const FSAllocator *allocator;
  uint64_t (*now_fn)(void *context);
  void *now_context;
  const FSTime *time_provider; /* NULL = use FSTime_init (platform default) */
} FeatherConfig;

typedef FSSchedulerComponentMemorySnapshot FeatherComponentMemorySnapshot;

typedef struct Feather {
  FSScheduler scheduler;
} Feather;

extern FEATHER_API const FeatherConfig FeatherConfig_init;

FEATHER_API bool Feather_init(struct Feather *feather);
FEATHER_API bool Feather_init_with_config(struct Feather *feather,
                                          const FeatherConfig *config);
FEATHER_API void Feather_deinit(struct Feather *feather);
FEATHER_API uint64_t Feather_add_instant_task(struct Feather *feather,
                                              FSSchedulerInstantTask task);
FEATHER_API uint64_t Feather_add_deferred_task(struct Feather *feather,
                                               FSSchedulerDeferredTask task);
FEATHER_API uint64_t Feather_add_repeating_task(struct Feather *feather,
                                                FSSchedulerRepeatingTask task);
FEATHER_API bool Feather_set_time_source(struct Feather *feather,
                                         uint64_t (*now_fn)(void *context),
                                         void *context);
FEATHER_API bool Feather_set_time_provider(struct Feather *feather,
                                           const FSTime *provider);
FEATHER_API bool Feather_has_pending_tasks(const struct Feather *feather);
FEATHER_API bool Feather_next_sleep_ms(const struct Feather *feather,
                                       uint64_t *out_delay_ms);
FEATHER_API bool Feather_process_for_ms(struct Feather *feather,
                                        uint64_t duration_ms);
FEATHER_API bool Feather_step(struct Feather *feather);
FEATHER_API FeatherComponentMemorySnapshot
Feather_component_memory_snapshot(const struct Feather *feather);
FEATHER_API bool Feather_cancel_task(struct Feather *feather,
                                     uint64_t task_id);
FEATHER_API FSSchedulerTaskStatus
Feather_task_status(const struct Feather *feather, uint64_t task_id);
FEATHER_API FSSchedulerStateSnapshot
Feather_state_snapshot(const struct Feather *feather);

FEATHER_EXTERN_C_END

#endif
