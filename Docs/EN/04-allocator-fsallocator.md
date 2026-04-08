# Module: Allocator Abstraction (`FSAllocator`)

## Purpose

`FSAllocator` provides runtime-configurable allocation hooks for Feather internals and extensions.

Main files:
- `System/FeatherRuntime/FSAllocator.hpp`
- `System/FeatherRuntime/FSAllocator.cpp`

## Public Type

### `FSAllocator`
Fields:
- `context`
- `allocate(context, size)`
- `reallocate(context, pointer, size)`
- `deallocate(context, pointer)`

## Exported constant

- `FSAllocator_system`: default system allocator (`malloc/realloc/free` behavior).

## Complete Public Function Reference

- `const FSAllocator *FSAllocator_resolve(const FSAllocator *allocator)`
- `void *FSAllocator_allocate(const FSAllocator *allocator, size_t size)`
- `void *FSAllocator_reallocate(const FSAllocator *allocator, void *pointer, size_t size)`
- `void FSAllocator_deallocate(const FSAllocator *allocator, void *pointer)`

## Behavior Notes

- `FSAllocator_resolve` falls back to `FSAllocator_system` if allocator is `NULL` or incomplete.
- `size == 0` for allocate/reallocate returns `NULL` (and frees on reallocate path).
- Wrappers always apply resolve logic before invoking hooks.

## Usage Example

```c
#include "FeatherRuntime/FSAllocator.hpp"
#include <stdlib.h>

typedef struct CountingCtx {
    size_t alloc_calls;
} CountingCtx;

static void *my_alloc(void *ctx, size_t size) {
    ((CountingCtx *)ctx)->alloc_calls += 1;
    return malloc(size);
}

static void *my_realloc(void *ctx, void *ptr, size_t size) {
    (void)ctx;
    return realloc(ptr, size);
}

static void my_free(void *ctx, void *ptr) {
    (void)ctx;
    free(ptr);
}

int main(void) {
    CountingCtx ctx = {0};
    FSAllocator custom = {
        .context = &ctx,
        .allocate = my_alloc,
        .reallocate = my_realloc,
        .deallocate = my_free,
    };

    void *p = FSAllocator_allocate(&custom, 64);
    p = FSAllocator_reallocate(&custom, p, 128);
    FSAllocator_deallocate(&custom, p);
    return 0;
}
```
