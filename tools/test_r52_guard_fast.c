// Compile: clang -std=c23 -O2 -Isrc tools/test_r52_guard_fast.c -o /tmp/r52guard
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "port/r52_fast_guard.h"
static Wram w,copy;
static uint32_t seed=0x528480;
static uint32_t rnd(void) { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; }
static void set_handler(uint16_t slot, uint32_t addr) {
  wram_w16(&w, W_THREAD_HANDLER + slot, addr & 0xffff);
  wram_w16(&w, W_THREAD_HANDLER_BANK + slot, (addr >> 16) & 0xff);
}
static uint32_t entries[] = {
  PLAYER_COLLIDE_ENTRY, ENEMY_COLLIDE_ENTRY, MONSTER_COLLIDE_ENTRY, MONSTER_C440_COLLIDE_ENTRY, ENEMY_B41C_COLLIDE_ENTRY, ENEMY_CDDE_COLLIDE_ENTRY, ENEMY_B592_COLLIDE_ENTRY, ENEMY_D7F6_COLLIDE_ENTRY, ENEMY_9A6D_COLLIDE_ENTRY, ENEMY_9B6B_COLLIDE_ENTRY, ENEMY_9063_COLLIDE_ENTRY, ENEMY_D301_COLLIDE_ENTRY, ENEMY_AC92_COLLIDE_ENTRY, ENEMY_E6E4_COLLIDE_ENTRY, ENEMY_990B_COLLIDE_ENTRY, ENEMY_990B_SPIN_ENTRY, ACTOR_845E_COLLIDE_ENTRY, ACTOR_DEEB_COLLIDE_ENTRY, ACTOR_F1C2_COLLIDE_ENTRY, ACTOR_F534_COLLIDE_ENTRY, VICTIM_A264_COLLIDE_ENTRY, BOSS_9660_COLLIDE_ENTRY, BOSS_AA2E_COLLIDE_ENTRY, ACTOR_F330_COLLIDE_ENTRY, ACTOR_A638_COLLIDE_ENTRY, ACTOR_84AC_COLLIDE_ENTRY, ENEMY_B95F_COLLIDE_ENTRY, ENEMY_EFF0_COLLIDE_ENTRY, ACTOR_C8C3_COLLIDE_ENTRY, SHOT_COLLIDE_ENTRY, SHOT_EDAA_COLLIDE_ENTRY, SHOT_F6A3_COLLIDE_ENTRY, ACTOR_F4EF_COLLIDE_ENTRY, VICTIM_COLLIDE_ENTRY, OBJECT_COLLIDE_ENTRY
};
int main(void) {
  for (int t=0;t<60000;t++) {
    // The port's absolute thread table is indexed by even slots 0..46.
    for (int i=0;i<WRAM_SIZE;i++) w.bytes[i]=(uint8_t)rnd();
    const uint16_t slot=(uint16_t)(2*(rnd()%24));
    uint32_t addr=0;
    R52GuardDecision expect;
    switch(t%4) {
      case 0: expect=R52_APPROVE_NO_HANDLER;break;
      case 1: {
        const uint32_t total[]={SHOT_EDAA_COLLIDE_ENTRY, SHOT_F6A3_COLLIDE_ENTRY,
          ENEMY_CDDE_COLLIDE_ENTRY, ENEMY_B592_COLLIDE_ENTRY, ACTOR_845E_COLLIDE_ENTRY};
        addr=total[rnd()%5];expect=R52_APPROVE_TOTAL_HANDLER;break;
      }
      case 2: {
        addr=entries[rnd()%(sizeof(entries)/sizeof(entries[0]))];
        expect=(addr==SHOT_EDAA_COLLIDE_ENTRY || addr==SHOT_F6A3_COLLIDE_ENTRY ||
                addr==ENEMY_CDDE_COLLIDE_ENTRY || addr==ENEMY_B592_COLLIDE_ENTRY ||
                addr==ACTOR_845E_COLLIDE_ENTRY)?R52_APPROVE_TOTAL_HANDLER:R52_DEFER_TO_SANDBOX;
        break;
      }
      default: addr=0x7a9000u|((rnd()&0x3fffu));expect=R52_REJECT_UNPORTED;break;
    }
    set_handler(slot,addr);
    copy=w;
    uint32_t missing=0;
    R52GuardDecision got=r52_handler_slot_decision(&w,slot,&missing);
    assert(got==expect);
    if(got==R52_REJECT_UNPORTED)assert(missing==addr);
    assert(memcmp(&w,&copy,sizeof w)==0);
    // First notification can mutate the second's registration. A known first
    // handler must always DEFER even when second handler is unknown.
    const uint16_t b=0x185eu, a=0x1872u;
    const uint16_t sb=2,sa=4;
    wram_w16(&w,b+ACTOR_THREAD,sb);
    wram_w16(&w,a+ACTOR_THREAD,sa);
    uint32_t known=entries[rnd()%(sizeof(entries)/sizeof(entries[0]))];
    uint32_t unknown=0x7a7777u;
    R52GuardDecision ne;
    switch(t%7) {
      case 0: set_handler(sb,0);set_handler(sa,0);ne=R52_APPROVE_NO_HANDLER;break;
      case 1: set_handler(sb,0);set_handler(sa,unknown);ne=R52_REJECT_UNPORTED;break;
      case 2: set_handler(sb,known);set_handler(sa,unknown);ne=R52_DEFER_TO_SANDBOX;break;
      case 3: set_handler(sb,unknown);set_handler(sa,0);ne=R52_REJECT_UNPORTED;break;
      case 4:set_handler(sb,0);set_handler(sa,known);
        ne=(known==SHOT_EDAA_COLLIDE_ENTRY || known==SHOT_F6A3_COLLIDE_ENTRY ||
            known==ENEMY_CDDE_COLLIDE_ENTRY || known==ENEMY_B592_COLLIDE_ENTRY ||
            known==ACTOR_845E_COLLIDE_ENTRY)?R52_APPROVE_TOTAL_HANDLER:R52_DEFER_TO_SANDBOX;
        break;
      case 5:set_handler(sb,SHOT_EDAA_COLLIDE_ENTRY);set_handler(sa,unknown);
        ne=R52_DEFER_TO_SANDBOX;break;
      default:set_handler(sb,0);set_handler(sa,SHOT_EDAA_COLLIDE_ENTRY);ne=R52_APPROVE_TOTAL_HANDLER;break;
    }
    copy=w;
    assert(r52_notify_decision(&w,a,b)==ne);
    assert(memcmp(&w,&copy,sizeof w)==0);
  }
  printf("PASS 60000 thread + 60000 notify fast-guard cases; 35 handler addresses; 5 total handlers; no writes; first-handler mutation cases defer\n");
}
