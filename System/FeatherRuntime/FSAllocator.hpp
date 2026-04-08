#ifndef FS_ALLOCATOR_H
#define FS_ALLOCATOR_H

#include "../FeatherExport.hpp"

#include <stddef.h>

typedef struct FSAllocator {
  void *context;
  void *(*allocate)(void *context, size_t size);
  void *(*reallocate)(void *context, void *pointer, size_t size);
  void (*deallocate)(void *context, void *pointer);
} FSAllocator;

FEATHER_EXTERN_C_BEGIN

extern FEATHER_API const FSAllocator FSAllocator_system;

FEATHER_API const FSAllocator *FSAllocator_resolve(
    const FSAllocator *allocator);
FEATHER_API void *FSAllocator_allocate(const FSAllocator *allocator,
                                       size_t size);
FEATHER_API void *FSAllocator_reallocate(const FSAllocator *allocator,
                                         void *pointer, size_t size);
FEATHER_API void FSAllocator_deallocate(const FSAllocator *allocator,
                                        void *pointer);

FEATHER_EXTERN_C_END

#endif
