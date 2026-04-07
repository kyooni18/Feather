#ifndef FS_RESOURCE_TRACKER_H
#define FS_RESOURCE_TRACKER_H

#include "../FeatherExport.h"
#include "FSAllocator.h"
#include "FSScheduler.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct FSResourceTrackerRecord {
  void *pointer;
  size_t size;
  uint64_t allocated_at_ms;
} FSResourceTrackerRecord;

typedef struct FSResourceTrackerSnapshot {
  size_t current_bytes;
  size_t peak_bytes;
  size_t total_allocated_bytes;
  size_t total_freed_bytes;
  size_t active_allocations;
} FSResourceTrackerSnapshot;

typedef struct FSResourceTrackerConfig {
  const FSAllocator *base_allocator;
  uint64_t (*now_fn)(void *context);
  void *now_context;
} FSResourceTrackerConfig;

typedef struct FSResourceTracker {
  const FSAllocator *base_allocator;
  uint64_t (*now_fn)(void *context);
  void *now_context;
  FSAllocator allocator;
  FSResourceTrackerRecord *records;
  size_t record_count;
  size_t record_capacity;
  size_t current_bytes;
  size_t peak_bytes;
  size_t total_allocated_bytes;
  size_t total_freed_bytes;
  bool initialized;
} FSResourceTracker;

typedef struct FSResourceTrackerSchedulerSnapshot {
  FSResourceTrackerSnapshot memory;
  FSSchedulerStateSnapshot  scheduler;
} FSResourceTrackerSchedulerSnapshot;

FEATHER_EXTERN_C_BEGIN

extern FEATHER_API const FSResourceTrackerConfig FSResourceTrackerConfig_init;

FEATHER_API bool FSResourceTracker_init(FSResourceTracker *tracking);
FEATHER_API bool FSResourceTracker_init_with_config(
    FSResourceTracker *tracking, const FSResourceTrackerConfig *config);
FEATHER_API void FSResourceTracker_deinit(FSResourceTracker *tracking);
FEATHER_API const FSAllocator *FSResourceTracker_allocator(
    const FSResourceTracker *tracking);
FEATHER_API FSResourceTrackerSnapshot FSResourceTracker_snapshot(
    const FSResourceTracker *tracking);
FEATHER_API size_t FSResourceTracker_copy_active_records(
    const FSResourceTracker *tracking, FSResourceTrackerRecord *out_records,
    size_t max_records);
FEATHER_API bool FSResourceTracker_has_leaks(
    const FSResourceTracker *tracking);
FEATHER_API FSResourceTrackerSchedulerSnapshot
FSResourceTracker_scheduler_snapshot(const FSResourceTracker *tracking,
                                     const FSScheduler *scheduler);

FEATHER_EXTERN_C_END

#endif
