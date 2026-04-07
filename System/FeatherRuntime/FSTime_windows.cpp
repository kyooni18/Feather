/* Windows-specific FSTime backend.
 * Monotonic time: QueryPerformanceCounter (hardware tick counter).
 * Wall-clock time: GetSystemTimeAsFileTime converted to Unix milliseconds.
 * Sleep: Sleep() Win32 API.
 * Compiled only on _WIN32 targets.
 */

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "FSTime.h"

uint64_t FSTime_now_monotonic(void) {
  LARGE_INTEGER frequency, counter;
  if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0)
    return 0;
  if (!QueryPerformanceCounter(&counter))
    return 0;
  return (uint64_t)(counter.QuadPart / frequency.QuadPart) * 1000ULL +
         (uint64_t)((counter.QuadPart % frequency.QuadPart) * 1000LL /
                    frequency.QuadPart);
}

uint64_t FSTime_now_unix(void) {
  FILETIME ft;
  ULARGE_INTEGER ticks;
  const uint64_t epoch = 116444736000000000ULL;
  GetSystemTimeAsFileTime(&ft);
  ticks.LowPart  = ft.dwLowDateTime;
  ticks.HighPart = ft.dwHighDateTime;
  return ticks.QuadPart < epoch ? 0 : (ticks.QuadPart - epoch) / 10000ULL;
}

bool FSTime_sleep_ms(uint64_t duration_ms) {
  if (duration_ms == 0) return true;
  Sleep((DWORD)(duration_ms > (uint64_t)(INFINITE - 1) ? INFINITE - 1
                                                       : duration_ms));
  return true;
}

#endif /* _WIN32 */
