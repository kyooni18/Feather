/* Arduino-specific FSTime backend.
 * Uses millis() for both monotonic and wall-clock time, and delay() for
 * sleeping.  Compiled only when the ARDUINO toolchain is active.
 */

#ifdef ARDUINO
#include "FSTime.h"

#include <Arduino.h>

uint64_t FSTime_now_monotonic(void) {
  return (uint64_t)millis();
}

uint64_t FSTime_now_unix(void) {
  return (uint64_t)millis();
}

bool FSTime_sleep_ms(uint64_t duration_ms) {
  if (duration_ms == 0) return true;
  delay((unsigned long)duration_ms);
  return true;
}

#endif /* ARDUINO */
