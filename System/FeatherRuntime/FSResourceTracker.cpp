#include "FSResourceTracker.hpp"

#include <string.h>

#include "FSTime.hpp"

#define FSResourceTracker_INITIAL_CAPACITY 16

static uint64_t FSResourceTracker_default_now_ms(void *context) {
  (void)context;
  return FSTime_now_monotonic();
}

static void *FSResourceTracker_allocate_impl(void *context, size_t size);
static void *FSResourceTracker_reallocate_impl(void *context, void *pointer,
                                                size_t size);
static void FSResourceTracker_deallocate_impl(void *context, void *pointer);

static const FSAllocator *FSResourceTracker_base_allocator(
    const FSResourceTracker *tracking) {
  if (tracking == NULL) {
    return FSAllocator_resolve(NULL);
  }

  return FSAllocator_resolve(tracking->base_allocator);
}

static bool FSResourceTracker_ensure_capacity(FSResourceTracker *tracking) {
  size_t new_capacity;
  void *new_records;

  if (tracking == NULL) {
    return false;
  }

  if (tracking->record_count < tracking->record_capacity) {
    return true;
  }

  new_capacity = (tracking->record_capacity > 0)
                     ? tracking->record_capacity * 2
                     : (size_t)FSResourceTracker_INITIAL_CAPACITY;
  new_records = FSAllocator_reallocate(
      tracking->base_allocator, tracking->records,
      new_capacity * sizeof(FSResourceTrackerRecord));
  if (new_records == NULL) {
    return false;
  }

  tracking->records = (FSResourceTrackerRecord *)new_records;
  tracking->record_capacity = new_capacity;
  return true;
}

static size_t FSResourceTracker_find_record_index(
    const FSResourceTracker *tracking, void *pointer) {
  size_t i;

  if (tracking == NULL || pointer == NULL) {
    return tracking != NULL ? tracking->record_count : 0;
  }

  for (i = 0; i < tracking->record_count; i++) {
    if (tracking->records[i].pointer == pointer) {
      return i;
    }
  }

  return tracking->record_count;
}

static bool FSResourceTracker_track_pointer(FSResourceTracker *tracking,
                                             void *pointer, size_t size) {
  FSResourceTrackerRecord *record;

  if (tracking == NULL || pointer == NULL || size == 0) {
    return false;
  }

  if (!FSResourceTracker_ensure_capacity(tracking)) {
    return false;
  }

  record = &tracking->records[tracking->record_count];
  record->pointer = pointer;
  record->size = size;
  record->allocated_at_ms = tracking->now_fn(tracking->now_context);
  tracking->record_count++;
  tracking->current_bytes += size;
  tracking->total_allocated_bytes += size;

  if (tracking->current_bytes > tracking->peak_bytes) {
    tracking->peak_bytes = tracking->current_bytes;
  }

  return true;
}

static void *FSResourceTracker_allocate_impl(void *context, size_t size) {
  FSResourceTracker *tracking = (FSResourceTracker *)context;
  void *pointer;

  if (tracking == NULL || size == 0) {
    return NULL;
  }

  if (!tracking->initialized) {
    return FSAllocator_allocate(FSResourceTracker_base_allocator(tracking),
                                size);
  }

  pointer = FSAllocator_allocate(FSResourceTracker_base_allocator(tracking),
                                 size);
  if (pointer == NULL) {
    return NULL;
  }

  if (!FSResourceTracker_track_pointer(tracking, pointer, size)) {
    FSAllocator_deallocate(tracking->base_allocator, pointer);
    return NULL;
  }

  return pointer;
}

static void *FSResourceTracker_reallocate_impl(void *context, void *pointer,
                                                size_t size) {
  FSResourceTracker *tracking = (FSResourceTracker *)context;
  size_t record_index;
  size_t previous_size = 0;
  void *new_pointer;

  if (tracking == NULL) {
    return NULL;
  }

  if (pointer == NULL) {
    return FSResourceTracker_allocate_impl(context, size);
  }

  if (!tracking->initialized) {
    return FSAllocator_reallocate(FSResourceTracker_base_allocator(tracking),
                                  pointer, size);
  }

  if (size == 0) {
    FSResourceTracker_deallocate_impl(context, pointer);
    return NULL;
  }

  record_index = FSResourceTracker_find_record_index(tracking, pointer);
  if (record_index < tracking->record_count) {
    previous_size = tracking->records[record_index].size;
  }

  new_pointer = FSAllocator_reallocate(FSResourceTracker_base_allocator(tracking),
                                       pointer, size);
  if (new_pointer == NULL) {
    return NULL;
  }

  if (record_index >= tracking->record_count) {
    if (!FSResourceTracker_track_pointer(tracking, new_pointer, size)) {
      FSAllocator_deallocate(FSResourceTracker_base_allocator(tracking),
                             new_pointer);
      return NULL;
    }
    return new_pointer;
  }

  tracking->records[record_index].pointer = new_pointer;
  tracking->records[record_index].size = size;

  if (size > previous_size) {
    size_t delta = size - previous_size;
    tracking->current_bytes += delta;
    tracking->total_allocated_bytes += delta;
  } else if (previous_size > size) {
    size_t delta = previous_size - size;
    tracking->current_bytes -= delta;
    tracking->total_freed_bytes += delta;
  }

  if (tracking->current_bytes > tracking->peak_bytes) {
    tracking->peak_bytes = tracking->current_bytes;
  }

  return new_pointer;
}

static void FSResourceTracker_deallocate_impl(void *context, void *pointer) {
  FSResourceTracker *tracking = (FSResourceTracker *)context;
  size_t record_index;
  size_t size = 0;

  if (tracking == NULL || pointer == NULL) {
    return;
  }

  if (!tracking->initialized) {
    FSAllocator_deallocate(FSResourceTracker_base_allocator(tracking), pointer);
    return;
  }

  record_index = FSResourceTracker_find_record_index(tracking, pointer);
  if (record_index < tracking->record_count) {
    size = tracking->records[record_index].size;
    tracking->current_bytes -= size;
    tracking->total_freed_bytes += size;
    tracking->record_count--;
    if (record_index < tracking->record_count) {
      tracking->records[record_index] = tracking->records[tracking->record_count];
    }
  }

  FSAllocator_deallocate(FSResourceTracker_base_allocator(tracking), pointer);
}

const FSResourceTrackerConfig FSResourceTrackerConfig_init = {
    .base_allocator = NULL, .now_fn = NULL, .now_context = NULL};

bool FSResourceTracker_init(FSResourceTracker *tracking) {
  return FSResourceTracker_init_with_config(tracking,
                                             &FSResourceTrackerConfig_init);
}

bool FSResourceTracker_init_with_config(
    FSResourceTracker *tracking, const FSResourceTrackerConfig *config) {
  const FSResourceTrackerConfig *resolved_config =
      (config != NULL) ? config : &FSResourceTrackerConfig_init;

  if (tracking == NULL) {
    return false;
  }

  memset(tracking, 0, sizeof(*tracking));
  tracking->base_allocator = FSAllocator_resolve(resolved_config->base_allocator);
  tracking->now_fn = (resolved_config->now_fn != NULL)
                         ? resolved_config->now_fn
                         : FSResourceTracker_default_now_ms;
  tracking->now_context = resolved_config->now_context;
  tracking->allocator = (FSAllocator){
      .context = tracking,
      .allocate = FSResourceTracker_allocate_impl,
      .reallocate = FSResourceTracker_reallocate_impl,
      .deallocate = FSResourceTracker_deallocate_impl};
  tracking->initialized = true;
  return true;
}

void FSResourceTracker_deinit(FSResourceTracker *tracking) {
  if (tracking == NULL) {
    return;
  }

  FSAllocator_deallocate(FSResourceTracker_base_allocator(tracking),
                         tracking->records);
  tracking->records = NULL;
  tracking->record_count = 0;
  tracking->record_capacity = 0;
  tracking->current_bytes = 0;
  tracking->peak_bytes = 0;
  tracking->total_allocated_bytes = 0;
  tracking->total_freed_bytes = 0;
  tracking->initialized = false;
}

const FSAllocator *FSResourceTracker_allocator(
    const FSResourceTracker *tracking) {
  if (tracking == NULL || !tracking->initialized) {
    return NULL;
  }

  return &tracking->allocator;
}

FSResourceTrackerSnapshot FSResourceTracker_snapshot(
    const FSResourceTracker *tracking) {
  FSResourceTrackerSnapshot snapshot = {0, 0, 0, 0, 0};

  if (tracking == NULL) {
    return snapshot;
  }

  snapshot.current_bytes = tracking->current_bytes;
  snapshot.peak_bytes = tracking->peak_bytes;
  snapshot.total_allocated_bytes = tracking->total_allocated_bytes;
  snapshot.total_freed_bytes = tracking->total_freed_bytes;
  snapshot.active_allocations = tracking->record_count;
  return snapshot;
}

size_t FSResourceTracker_copy_active_records(
    const FSResourceTracker *tracking, FSResourceTrackerRecord *out_records,
    size_t max_records) {
  size_t copy_count;

  if (tracking == NULL) {
    return 0;
  }

  copy_count = tracking->record_count;
  if (out_records == NULL || max_records == 0) {
    return copy_count;
  }

  if (copy_count > max_records) {
    copy_count = max_records;
  }

  memcpy(out_records, tracking->records,
         copy_count * sizeof(FSResourceTrackerRecord));
  return tracking->record_count;
}

bool FSResourceTracker_has_leaks(const FSResourceTracker *tracking) {
  return tracking != NULL && tracking->record_count > 0;
}


FSResourceTrackerSchedulerSnapshot FSResourceTracker_scheduler_snapshot(
    const FSResourceTracker *tracking, const FSScheduler *scheduler) {
  FSResourceTrackerSchedulerSnapshot snapshot;

  snapshot.memory    = FSResourceTracker_snapshot(tracking);
  snapshot.scheduler = FSScheduler_state_snapshot(scheduler);

  return snapshot;
}
