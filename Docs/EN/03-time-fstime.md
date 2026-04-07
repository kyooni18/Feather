# Module: Time Layer (`FSTime`)

## Purpose

`FSTime` abstracts clock and sleeping operations for portability.

Main files:
- `System/FeatherRuntime/FSTime.h`
- `System/FeatherRuntime/FSTime.cpp`
- `System/FeatherRuntime/FSTime_posix.cpp`
- `System/FeatherRuntime/FSTime_windows.cpp`
- `System/FeatherRuntime/FSTime_arduino.cpp`

## Public Type

### `FSTime`
Provider struct with callbacks:
- `now_monotonic_ms(void)`
- `now_unix_ms(void)`
- `sleep_ms(uint64_t duration_ms)`

## Exported constant

- `FSTime_init`: platform-default provider.

## Complete Public Function Reference

- `uint64_t FSTime_now_monotonic(void)`
- `uint64_t FSTime_now_unix(void)`
- `bool FSTime_sleep_ms(uint64_t duration_ms)`

These wrappers use the currently configured provider default (`FSTime_init`).

## Platform Behavior Summary

- POSIX: `clock_gettime` + `nanosleep`.
- Windows: high-resolution performance counter + file-time conversion + `Sleep`.
- Arduino: `millis()` and `delay()`.

## Usage Example

```c
#include "FeatherRuntime/FSTime.h"
#include <stdio.h>

int main(void) {
    uint64_t t0 = FSTime_now_monotonic();
    (void)FSTime_sleep_ms(10);
    uint64_t t1 = FSTime_now_monotonic();
    uint64_t unix_ms = FSTime_now_unix();

    printf("elapsed=%llu ms, unix=%llu ms\n",
           (unsigned long long)(t1 - t0),
           (unsigned long long)unix_ms);
    return 0;
}
```
