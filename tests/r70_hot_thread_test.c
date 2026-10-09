// R70: exercise the real $82:9A6D callback and its conservative fast verdict.
// clang -O2 -std=c11 -ffunction-sections -fdata-sections -Isrc \
//   tests/r70_hot_thread_test.c src/port/collide.c src/assets/rom.c \
//   -Wl,--gc-sections -o /tmp/r70guard && /tmp/r70guard
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "port/r52_fast_guard.h"
static Wram ram, old;
static unsigned rng=0x70ab91cbu;
static unsigned next(void){rng ^=rng<<13; rng ^=rng>>17; rng ^=rng<<5;return rng;}
static void handler_set(unsigned slot, uint32_t e) {
 wram_w16(&ram,W_THREAD_HANDLER +slot,(uint16_t)e);
 wram_w16(&ram,W_THREAD_HANDLER_BANK +slot,(uint16_t)(e>>16));
}
int main(void) {
 static uint8_t empty[1048576];
 const Rom rom={empty,sizeof empty};
 assert(COLLIDE_ID_PLAYER==0x005cu);
 for (unsigned k=0;k<20000;k++) {
  for (unsigned i=0;i<WRAM_SIZE;i++) ram.bytes[i]=(uint8_t)next();
  const uint16_t slot=(uint16_t)(2u*(next()%24u));
  const uint16_t arg=(uint16_t)(next()%COLLIDE_ID_PLAYER);
  const uint16_t dp=(uint16_t)(next()%0x1f00u);
  handler_set(slot,ENEMY_9A6D_COLLIDE_ENTRY);
  old=ram;
  assert(r52_handler_slot_decision(&ram,slot,NULL)==R52_DEFER_TO_SANDBOX);
  assert(r68_thread_input_decision(&ram,slot,arg)==R52_DEFER_TO_SANDBOX);
  assert(r70_thread_hot_ignore_decision(&ram,slot,arg)==R70_APPROVE_9A6D_IGNORE);
  ActorHandlerRegs regs={.a=(uint16_t)next(),.x=(uint16_t)next(),
         .y=(uint16_t)next(),.n=(next()&1)!=0,.z=(next()&1)!=0,.c=(next()&1)!=0};
  assert(enemy_d7f6_collide(&ram,&rom,dp,arg,&regs,NULL));
  assert(!memcmp(&ram,&old,sizeof ram));
  assert(regs.a==arg && regs.c==false && regs.n==true && regs.z==false);
  assert(r70_thread_hot_ignore_decision(&ram,slot,COLLIDE_ID_PLAYER)==R52_DEFER_TO_SANDBOX);
  assert(r70_thread_hot_ignore_decision(&ram,slot,0xffff)==R52_DEFER_TO_SANDBOX);
  handler_set(slot,ENEMY_D7F6_COLLIDE_ENTRY);
  assert(r70_thread_hot_ignore_decision(&ram,slot,arg)==R52_DEFER_TO_SANDBOX);
  handler_set(slot,0);
  assert(r70_thread_hot_ignore_decision(&ram,slot,arg)==R52_DEFER_TO_SANDBOX);
 }
 puts("R70 HOT THREAD PASS 20,000 full 128KiB WRAM-stability actual-callback tests plus negative cases");
 return 0;
}
