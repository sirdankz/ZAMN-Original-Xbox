// Host-only regression: cc -std=c11 -O2 -ffunction-sections -fdata-sections -Isrc \
//   -DZAMN_R66_TOTAL_HANDLER_FAST_GUARDS=1 tests/r66_total_handler_guard_test.c \
//   src/port/collide.c -Wl,--gc-sections -o /tmp/r66total && /tmp/r66total
// Checks the actual C callback implementations, not just a mocked dispatcher.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "port/r52_fast_guard.h"
#include "port/cheat.h"

typedef bool (*TotalHandler)(Wram*, uint16_t, uint16_t, ActorHandlerRegs*);
static bool call_deeb(Wram* w,uint16_t dp,uint16_t arg,ActorHandlerRegs* r){return actor_deeb_collide(w,dp,arg,r);}
static bool call_f1c2(Wram* w,uint16_t dp,uint16_t arg,ActorHandlerRegs* r){return actor_f1c2_collide(w,dp,arg,r);}
static bool call_f534(Wram* w,uint16_t dp,uint16_t arg,ActorHandlerRegs* r){return actor_f534_collide(w,dp,arg,r);}
static bool call_a264(Wram* w,uint16_t dp,uint16_t arg,ActorHandlerRegs* r){return victim_a264_collide(w,dp,arg,r);}
static bool call_f330(Wram* w,uint16_t dp,uint16_t arg,ActorHandlerRegs* r){return actor_f330_collide(w,dp,arg,r);}
static bool call_a638(Wram* w,uint16_t dp,uint16_t arg,ActorHandlerRegs* r){return actor_a638_collide(w,dp,arg,r);}
static bool call_84ac(Wram* w,uint16_t dp,uint16_t arg,ActorHandlerRegs* r){return actor_84ac_collide(w,dp,arg,r);}
static bool call_f4ef(Wram* w,uint16_t dp,uint16_t arg,ActorHandlerRegs* r){return actor_f4ef_collide(w,dp,arg,r);}
static bool call_object(Wram* w,uint16_t dp,uint16_t arg,ActorHandlerRegs* r){return object_collide(w,dp,arg,r);}
static const struct { uint32_t addr; TotalHandler call; } new_total[] = {
 {ACTOR_DEEB_COLLIDE_ENTRY,call_deeb},{ACTOR_F1C2_COLLIDE_ENTRY,call_f1c2},
 {ACTOR_F534_COLLIDE_ENTRY,call_f534},{VICTIM_A264_COLLIDE_ENTRY,call_a264},
 {ACTOR_F330_COLLIDE_ENTRY,call_f330},{ACTOR_A638_COLLIDE_ENTRY,call_a638},
 {ACTOR_84AC_COLLIDE_ENTRY,call_84ac},{ACTOR_F4EF_COLLIDE_ENTRY,call_f4ef},
 {OBJECT_COLLIDE_ENTRY,call_object}
};
static Wram w, snapshot;
static uint32_t seed=0x661345b2u;
static uint32_t rnd(void){seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;return seed;}
static void assign(uint16_t slot,uint32_t addr) {
 wram_w16(&w,W_THREAD_HANDLER+slot,(uint16_t)addr);
 wram_w16(&w,W_THREAD_HANDLER_BANK+slot,(uint16_t)(addr>>16));
}
int main(void){
  const size_t n=sizeof new_total/sizeof new_total[0];
  unsigned long long executed=0,notify=0,first_deferred=0;
  for(unsigned t=0;t<27000;t++){
    // Overwrite EVERY WRAM byte, including actor/thread/DP region.
    for(unsigned i=0;i<WRAM_SIZE;i++) w.bytes[i]=(uint8_t)rnd();
    const unsigned which=t%n;
    const uint16_t slot=(uint16_t)(2*(rnd()%24));
    const uint16_t dp=(uint16_t)(0x80u*(rnd()%24));
    const uint16_t arg=(t%8==0)?0: (t%8==1)?0xffffu:
      (t%8==2)?0xffu:(t%8==3)?0x5cu:(t%8==4)?0x05u:
      (t%8==5)?0x06u:(uint16_t)rnd();
    assign(slot,new_total[which].addr);
    snapshot=w;
    assert(r52_handler_slot_decision(&w,slot,0)==R66_APPROVE_NEW_TOTAL);
    assert(!memcmp(&w,&snapshot,sizeof w));
    // Verify the real function is total even with arbitrary WRAM/register data.
    ActorHandlerRegs regs={0};
    regs.a=(uint16_t)rnd();regs.x=(uint16_t)rnd();regs.y=arg;
    regs.c=(rnd()&1)!=0;regs.n=(rnd()&1)!=0;regs.z=(rnd()&1)!=0;
    port_cheats.neighbors=(rnd()&1)!=0;
    assert(new_total[which].call(&w,dp,arg,&regs));
    executed++;
    // Never use a total-but-mutating FIRST callback to approve SECOND:
    // it can rewrite actor records and the thread handler registration.
    const uint16_t a=W_ACTOR_SLOTS+ACTOR_SLOT_STRIDE*3;
    const uint16_t b=W_ACTOR_SLOTS+ACTOR_SLOT_STRIDE*4;
    const uint16_t bs=slot;
    const uint16_t as=(uint16_t)((slot+2)%48);
    wram_w16(&w,b+ACTOR_THREAD,bs);
    wram_w16(&w,a+ACTOR_THREAD,as);
    assign(bs,new_total[which].addr);
    assign(as,0x7a9999u);snapshot=w;
    assert(r52_notify_decision(&w,a,b)==R52_DEFER_TO_SANDBOX);
    assert(r58_notify_decision(&w,a,b)==R52_DEFER_TO_SANDBOX);
    assert(!memcmp(&w,&snapshot,sizeof w));first_deferred++;
    // If FIRST has no callback, new total SECOND can skip the copy.
    assign(bs,0);assign(as,new_total[which].addr);snapshot=w;
    assert(r52_notify_decision(&w,a,b)==R66_APPROVE_NEW_TOTAL);
    assert(r58_notify_decision(&w,a,b)==R66_APPROVE_NEW_TOTAL);
    assert(!memcmp(&w,&snapshot,sizeof w));notify++;
    // R58's narrowly proven read-only first callback cannot modify the
    // second registration, so the second can also be R66-approved.
    assign(bs,SHOT_EDAA_COLLIDE_ENTRY);
    snapshot=w;
    assert(r58_notify_decision(&w,a,b)==R66_APPROVE_NEW_TOTAL);
    assert(!memcmp(&w,&snapshot,sizeof w));
  }
  printf("R66 PASS: %llu randomized real-C total-handler executions; %llu safe-second approvals; %llu mutating-first deferrals; full-WRAM read-only guard checks\n",executed,notify,first_deferred);
}
