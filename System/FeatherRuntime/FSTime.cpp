/* FSTime common layer.
 *
 * Platform-specific implementations live in the companion backend files:
 *   FSTime_posix.cpp    — Linux, macOS, ESP32, and other POSIX systems
 *   FSTime_windows.cpp  — Windows (_WIN32)
 *   FSTime_arduino.cpp  — Arduino (ARDUINO)
 *
 * This file only defines the FSTime_init singleton (which references the
 * public functions provided by exactly one of the backends above) and the
 * thin wrapper functions that delegate through the struct.
 */

#include "FSTime.hpp"

/* FSTime_now_monotonic, FSTime_now_unix, and FSTime_sleep_ms are defined in
 * the platform backend file compiled for this target. */
const FSTime FSTime_init = {
  .now_monotonic_ms = FSTime_now_monotonic,
  .now_unix_ms      = FSTime_now_unix,
  .sleep_ms         = FSTime_sleep_ms
};
