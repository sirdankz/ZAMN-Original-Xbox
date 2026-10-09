// R74: bounded, diagnostics-only profiler for the remaining collision preflights.
// No gameplay state or input changes. Limited to 16 distinct pair signatures.
#ifndef ZAMN_R74_PAIR_PROFILE_H
#define ZAMN_R74_PAIR_PROFILE_H
#include <stdint.h>
#include <string.h>
#define ZAMN_R74_PAIR_SLOTS 16
#define ZAMN_R74_PAIR_TOP 3
typedef struct {
  uint32_t first, second;
  uint16_t first_arg, second_arg;
  uint32_t calls, declined;
} ZamnR74PairStat;
static inline void r74_pair_stat_add(
    ZamnR74PairStat hist[ZAMN_R74_PAIR_SLOTS],
    uint32_t first, uint32_t second, uint16_t arg_first,
    uint16_t arg_second, int accepted, uint64_t* overflow) {
  for(unsigned i=0;i<ZAMN_R74_PAIR_SLOTS;i++) {
    ZamnR74PairStat* p=&hist[i];
    if (p->calls && (p->first != first || p->second != second ||
        p->first_arg != arg_first || p->second_arg != arg_second)) continue;
    p->first=first;p->second=second;
    p->first_arg=arg_first;p->second_arg=arg_second;
    if (p->calls < UINT32_MAX) ++p->calls;
    if (!accepted && p->declined < UINT32_MAX) ++p->declined;
    return;
  }
  if (overflow) ++*overflow;
}
static inline void r74_pair_stat_top(
    const ZamnR74PairStat hist[ZAMN_R74_PAIR_SLOTS],
    ZamnR74PairStat out[ZAMN_R74_PAIR_TOP]) {
  memset(out,0,ZAMN_R74_PAIR_TOP*sizeof(*out));
  for(unsigned i=0;i<ZAMN_R74_PAIR_SLOTS;i++) {
    const ZamnR74PairStat p=hist[i];
    if (!p.calls)continue;
    for(unsigned j=0;j<ZAMN_R74_PAIR_TOP;j++) {
      if (p.calls <= out[j].calls) continue;
      for(unsigned k=ZAMN_R74_PAIR_TOP-1;k>j;k--)out[k]=out[k-1];
      out[j]=p;break;
    }
  }
}
#endif
