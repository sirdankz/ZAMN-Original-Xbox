#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "port/r52_fast_guard.h"
static Wram w, original;
static unsigned seed=0x58BEEF42;
static unsigned rnd(void){seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;return seed;}
static void put(uint16_t slot,uint32_t entry){wram_w16(&w,W_THREAD_HANDLER+slot,(uint16_t)entry);wram_w16(&w,W_THREAD_HANDLER_BANK+slot,(uint16_t)(entry>>16));}
int main(void){
 const uint32_t handlers[]={0,SHOT_EDAA_COLLIDE_ENTRY,ACTOR_845E_COLLIDE_ENTRY,
 ENEMY_CDDE_COLLIDE_ENTRY,ENEMY_B592_COLLIDE_ENTRY,SHOT_F6A3_COLLIDE_ENTRY,
 PLAYER_COLLIDE_ENTRY,ENEMY_COLLIDE_ENTRY,0x7aaabb};
 int newly=0,rejected=0,deferred=0,unchanged=0;
 for(int t=0;t<24000;t++){
    for(unsigned i=0;i<WRAM_SIZE;i++)w.bytes[i]=(unsigned char)rnd();
    const uint16_t a=W_ACTOR_SLOTS+ACTOR_SLOT_STRIDE*(rnd()%16);
    const uint16_t b=W_ACTOR_SLOTS+ACTOR_SLOT_STRIDE*(16+rnd()%16);
    uint16_t sa=(uint16_t)(rnd()%24)*2, sb=(uint16_t)(rnd()%24)*2;
    if(sa==sb)sa=(uint16_t)((sa+2)%48);
    wram_w16(&w,a+ACTOR_THREAD,sa);wram_w16(&w,b+ACTOR_THREAD,sb);
    uint32_t be=handlers[rnd()%9],ae=handlers[rnd()%9];
    put(sb,be);put(sa,ae);original=w;
    R52GuardDecision old=r52_notify_decision(&w,a,b);
    R52GuardDecision got=r58_notify_decision(&w,a,b);
    assert(!memcmp(&w,&original,sizeof w));
    if(got==R58_APPROVE_READONLY_FIRST){
      assert(old==R52_DEFER_TO_SANDBOX);
      assert(be==SHOT_EDAA_COLLIDE_ENTRY || be==ACTOR_845E_COLLIDE_ENTRY);
      assert(ae==0 || ae==SHOT_EDAA_COLLIDE_ENTRY || ae==ACTOR_845E_COLLIDE_ENTRY ||
             ae==SHOT_F6A3_COLLIDE_ENTRY || ae==ENEMY_CDDE_COLLIDE_ENTRY || ae==ENEMY_B592_COLLIDE_ENTRY);
      newly++;
    }else if(got==R52_REJECT_UNPORTED){rejected++;}
    else if(got==R52_DEFER_TO_SANDBOX){deferred++;}
    else {assert(got==old);unchanged++;}
    if(old==R52_APPROVE_NO_HANDLER || old==R52_APPROVE_TOTAL_HANDLER || old==R52_REJECT_UNPORTED)
      assert(got==old);
 }
 assert(newly>0);
 printf("R58 notify READONLY-FIRST PASS cases=24000 newly-approved=%d rejected=%d deferred=%d same-old=%d\n",newly,rejected,deferred,unchanged);
}
