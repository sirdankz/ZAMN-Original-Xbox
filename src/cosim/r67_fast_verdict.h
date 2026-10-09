// R67: isolated fast-guard counter policy. The allow/deny decision is NOT
// changed: only decision 2 rejects; 1/3/4/5 approve. Decision 0 is
// handled by the caller's preflight (never passed into this helper).
#ifndef ZAMN_COSIM_R67_FAST_VERDICT_H
#define ZAMN_COSIM_R67_FAST_VERDICT_H
#include <stdint.h>
#include "port/r52_fast_guard.h"
static inline bool r67_count_fast_verdict(int decision, uint64_t* approved,
                                          uint64_t* rejected, uint64_t* declines) {
  if (decision == R52_REJECT_UNPORTED) {
    ++*rejected;
    ++*declines;
    return false;
  }
  ++*approved;
  return true;
}
#endif
