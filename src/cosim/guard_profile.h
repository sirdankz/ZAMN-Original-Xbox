#ifndef ZAMN_COSIM_GUARD_PROFILE_H
#define ZAMN_COSIM_GUARD_PROFILE_H
#include <stdint.h>
#define ZAMN_GUARD_PROFILE_TOP 4
typedef struct {
  const char* name;
  uint32_t pc;
  uint64_t calls;
  uint64_t bytes;
  uint64_t declines;
} ZamnGuardProfileHot;
#endif
