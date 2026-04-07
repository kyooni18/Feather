/* POSIX FSTime backend (Linux, macOS, ESP32, and other POSIX systems).
 *
 * Monotonic time: clock_gettime(CLOCK_MONOTONIC) — guaranteed never to go
 *   backward, unaffected by NTP adjustments or wall-clock changes.
 * Wall-clock time: clock_gettime(CLOCK_REALTIME) — Unix epoch milliseconds.
 * Sleep: nanosleep() — sub-millisecond precision POSIX sleep.
 *
 * Compiled on every target that is neither _WIN32 nor ARDUINO.
 */

#if !defined(_WIN32) && !defined(ARDUINO)

#define _POSIX_C_SOURCE 200809L

#include "FSTime.h"

#include <errno.h>
#include <stddef.h>
#include <time.h>

uint64_t FSTime_now_monotonic(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
  return (uint64_t)ts.tv_sec * 1000ULL +
         (uint64_t)(ts.tv_nsec / 1000000L);
}

uint64_t FSTime_now_unix(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return 0;
  return (uint64_t)ts.tv_sec * 1000ULL +
         (uint64_t)(ts.tv_nsec / 1000000L);
}

bool FSTime_sleep_ms(uint64_t duration_ms) {
  struct timespec ts;
  if (duration_ms == 0) return true;
  ts.tv_sec  = (time_t)(duration_ms / 1000ULL);
  ts.tv_nsec = (long)((duration_ms % 1000ULL) * 1000000ULL);
  /* Retry on EINTR so callers get the full requested sleep duration. */
  while (nanosleep(&ts, &ts) != 0) {
    if (errno != EINTR) return false;
  }
  return true;
}

#endif /* !_WIN32 && !ARDUINO */
