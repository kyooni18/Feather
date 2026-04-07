#ifndef FS_TIME_H
#define FS_TIME_H

#include "FeatherExport.hpp"

#include <stdbool.h>
#include <stdint.h>

typedef struct FSTime {
  uint64_t (*now_monotonic_ms)(void);
  uint64_t (*now_unix_ms)(void);
  bool (*sleep_ms)(uint64_t duration_ms);
} FSTime;

FEATHER_EXTERN_C_BEGIN

extern FEATHER_API const FSTime FSTime_init;

FEATHER_API uint64_t FSTime_now_monotonic(void);
FEATHER_API uint64_t FSTime_now_unix(void);
FEATHER_API bool FSTime_sleep_ms(uint64_t duration_ms);

FEATHER_EXTERN_C_END

#endif
