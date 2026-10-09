// R69 diagnostics-only OAM/thread guard hotspot attribution.
// Values are counters, NOT deterministic game state or a claim that an OAM
// failure belongs to its list head. OAM is intentionally unattributed.
#ifndef ZAMN_COSIM_R69_PROFILE_H
#define ZAMN_COSIM_R69_PROFILE_H
#include <stdint.h>
#define ZAMN_R69_GUARD_TOP 8
#define ZAMN_R69_GUARD_SLOTS 48
// handler_pc==0 denotes OAM or an unavailable callback. No ID is invented.
typedef struct {
  uint32_t guard_pc, handler_pc;
  uint16_t example_arg, example_db;
  uint64_t calls, bytes, declines;
} ZamnR69GuardHot;
#endif
