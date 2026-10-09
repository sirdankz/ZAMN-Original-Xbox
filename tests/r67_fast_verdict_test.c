// Host-only: clang -O2 -std=c11 -Isrc tests/r67_fast_verdict_test.c -o /tmp/r67-verdict && /tmp/r67-verdict
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "cosim/r67_fast_verdict.h"
int main(void) {
  uint64_t a=0,r=0,d=0;
  const int expected[]={R52_APPROVE_NO_HANDLER,R52_REJECT_UNPORTED,
                        R52_APPROVE_TOTAL_HANDLER,R58_APPROVE_READONLY_FIRST,
                        R66_APPROVE_NEW_TOTAL};
  for (int round=0;round<100000;++round) {
    for (unsigned i=0;i<sizeof(expected)/sizeof(expected[0]);++i) {
      const int x=expected[i];
      const uint64_t before_a=a,before_r=r,before_d=d;
      bool allowed=r67_count_fast_verdict(x,&a,&r,&d);
      assert(allowed==(x!=R52_REJECT_UNPORTED));
      assert(a-before_a==(x!=R52_REJECT_UNPORTED));
      assert(r-before_r==(x==R52_REJECT_UNPORTED));
      assert(d-before_d==(x==R52_REJECT_UNPORTED));
    }
  }
  assert(a==400000 && r==100000 && d==100000);
  puts("R67 PASS 500000 fast-verdict counter cases (approvals != rejects)");
  return 0;
}
