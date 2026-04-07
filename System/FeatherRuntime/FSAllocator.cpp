#include "FSAllocator.hpp"

#include <stdlib.h>

static void *FSAllocator_system_allocate_impl(void *context, size_t size) {
  (void)context;

  if (size == 0) {
    return NULL;
  }

  return malloc(size);
}

static void *FSAllocator_system_reallocate_impl(void *context, void *pointer,
                                                size_t size) {
  (void)context;

  if (pointer == NULL) {
    return FSAllocator_system_allocate_impl(NULL, size);
  }

  if (size == 0) {
    free(pointer);
    return NULL;
  }

  return realloc(pointer, size);
}

static void FSAllocator_system_deallocate_impl(void *context, void *pointer) {
  (void)context;
  free(pointer);
}

const FSAllocator FSAllocator_system = {
    .context = NULL,
    .allocate = FSAllocator_system_allocate_impl,
    .reallocate = FSAllocator_system_reallocate_impl,
    .deallocate = FSAllocator_system_deallocate_impl};

const FSAllocator *FSAllocator_resolve(const FSAllocator *allocator) {
  if (allocator == NULL || allocator->allocate == NULL ||
      allocator->reallocate == NULL || allocator->deallocate == NULL) {
    return &FSAllocator_system;
  }

  return allocator;
}

void *FSAllocator_allocate(const FSAllocator *allocator, size_t size) {
  const FSAllocator *resolved_allocator = FSAllocator_resolve(allocator);

  if (size == 0) {
    return NULL;
  }

  return resolved_allocator->allocate(resolved_allocator->context, size);
}

void *FSAllocator_reallocate(const FSAllocator *allocator, void *pointer,
                             size_t size) {
  const FSAllocator *resolved_allocator = FSAllocator_resolve(allocator);

  if (pointer == NULL) {
    return FSAllocator_allocate(resolved_allocator, size);
  }

  if (size == 0) {
    FSAllocator_deallocate(resolved_allocator, pointer);
    return NULL;
  }

  return resolved_allocator->reallocate(resolved_allocator->context, pointer,
                                        size);
}

void FSAllocator_deallocate(const FSAllocator *allocator, void *pointer) {
  const FSAllocator *resolved_allocator = FSAllocator_resolve(allocator);

  if (pointer == NULL) {
    return;
  }

  resolved_allocator->deallocate(resolved_allocator->context, pointer);
}
