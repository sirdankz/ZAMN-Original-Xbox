// Host regression for R68 input-aware, read-only native collision guards.
// cc -O2 -std=c11 -ffunction-sections -fdata-sections -Isrc \
//    -DZAMN_R66_TOTAL_HANDLER_FAST_GUARDS=1 -DZAMN_R68_INPUT_READONLY_GUARDS=1 \
//    tests/r68_input_readonly_guard_test.c src/port/collide.c src/port/oam.c \
//    src/assets/rom.c -Wl,--gc-sections -o /tmp/r68guard && /tmp/r68guard
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "port/r52_fast_guard.h"

typedef bool (*Call)(Wram*,uint16_t,uint16_t,ActorHandlerRegs*);
static const struct { uint32_t entry; Call call; } handlers[] = {
 {ACTOR_DEEB_COLLIDE_ENTRY,actor_deeb_collide},
 {ACTOR_A638_COLLIDE_ENTRY,actor_a638_collide},
 {ACTOR_F4EF_COLLIDE_ENTRY,actor_f4ef_collide},
 {ACTOR_F534_COLLIDE_ENTRY,actor_f534_collide},
 {ACTOR_F1C2_COLLIDE_ENTRY,actor_f1c2_collide},
 {ACTOR_84AC_COLLIDE_ENTRY,actor_84ac_collide}
};
static Wram w, prior;
static unsigned seed = 0x68c0ffeeu;
static uint32_t rnd(void){ seed ^= seed<<13; seed ^= seed>>17; seed ^= seed<<5; return seed; }
static void write_entry(uint16_t slot,uint32_t e){
 wram_w16(&w,W_THREAD_HANDLER+slot,(uint16_t)e);
 wram_w16(&w,W_THREAD_HANDLER_BANK+slot,(uint16_t)(e>>16));
}
static uint16_t safe_arg(uint32_t e){
 for(;;) { uint16_t a=(uint16_t)rnd(); if(a != 0 && r68_handler_readonly_for_arg(e,a))return a; }
}
static void test_callbacks(void){
 for (int t=0;t<36000;t++){
   for (unsigned j=0;j<WRAM_SIZE;j++) w.bytes[j]=(uint8_t)rnd();
   const unsigned i=(unsigned)t%6;
   const uint16_t a=safe_arg(handlers[i].entry);
   const uint16_t slot=(uint16_t)(2*(rnd()%24));
   const uint16_t dp=(uint16_t)rnd();
   write_entry(slot,handlers[i].entry);
   prior=w;
   assert(r68_thread_input_decision(&w,slot,a)==R68_APPROVE_READONLY_INPUT);
   assert(!memcmp(&w,&prior,sizeof w));
   ActorHandlerRegs r={(uint16_t)rnd(),(uint16_t)rnd(),(uint16_t)rnd(),
                      (rnd()&1)!=0,(rnd()&1)!=0,(rnd()&1)!=0};
   assert(handlers[i].call(&w,dp,a,&r));
   // This particular branch must not write a SINGLE byte, not even its wait flag.
   assert(!memcmp(&w,&prior,sizeof w));
   assert(!r.c);
 }
 // Negatives must NOT skip the old scratch verification.
 assert(!r68_handler_readonly_for_arg(ACTOR_DEEB_COLLIDE_ENTRY,DEEB_ID_STOP));
 assert(!r68_handler_readonly_for_arg(ACTOR_A638_COLLIDE_ENTRY,A638_ID_PARK));
 assert(!r68_handler_readonly_for_arg(ACTOR_F4EF_COLLIDE_ENTRY,F4EF_LATCH_P1));
 assert(!r68_handler_readonly_for_arg(ACTOR_F534_COLLIDE_ENTRY,F534_LATCH_A));
 assert(!r68_handler_readonly_for_arg(ACTOR_F1C2_COLLIDE_ENTRY,F1C2_ID_C));
 assert(!r68_handler_readonly_for_arg(ACTOR_84AC_COLLIDE_ENTRY,COLLIDE_ID_PLAYER));
 assert(!r68_handler_readonly_for_arg(ENEMY_COLLIDE_ENTRY,0));
}
static void put_rec(uint16_t rec, uint16_t next, uint16_t id, uint16_t thread){
 wram_w16(&w,rec+ACTOR_NEXT,next);
 wram_w16(&w,rec+ACTOR_FLAGS,ACTOR_DRAW|ACTOR_SCREEN_SPACE);
 wram_w16(&w,rec+ACTOR_X,120);
 wram_w16(&w,rec+ACTOR_Y,130);
 wram_w16(&w,rec+ACTOR_COLLIDE_ID,id);
 wram_w16(&w,rec+ACTOR_THREAD,thread);
 wram_w16(&w,rec+ACTOR_META,0); // no ROM graphics lookup
}
static void test_oam(void){
 static uint8_t rombytes[1048576];
 const Rom rom={rombytes,sizeof(rombytes)};
 const uint16_t a=W_ACTOR_SLOTS,b=W_ACTOR_SLOTS+ACTOR_SLOT_STRIDE;
 for(unsigned t=0;t<20000;t++){
   memset(&w,0,sizeof w);
   wram_w16(&w,W_ACTOR_LIST_HEAD,a);
   uint16_t id_a=(uint16_t)(t%2 ? 0x05 : 0x5c);
   uint16_t id_b=safe_arg(handlers[t%6].entry);
   if (id_a == id_b) id_a=(uint16_t)(id_a+1);
   put_rec(a,b,id_a,0);
   put_rec(b,0,id_b,2);
   write_entry(0,handlers[t%6].entry);
   write_entry(2,0);
   // a's callback receives b's collision id; b has no callback.
   assert(r68_handler_readonly_for_arg(handlers[t%6].entry,id_b));
   prior=w;
   assert(sprite_build_oam_r57_fast_decision(&w,&rom)==R68_APPROVE_READONLY_INPUT);
   assert(!memcmp(&prior,&w,sizeof w));
   // Unknown first/second handlers must NOT be promoted.
   write_entry(0,ENEMY_COLLIDE_ENTRY);
   assert(sprite_build_oam_r57_fast_decision(&w,&rom)==0);
   write_entry(0,handlers[t%6].entry);
   // Cyclic malformed list must defer, even if callbacks look safe.
   wram_w16(&w,b+ACTOR_NEXT,a);
   assert(sprite_build_oam_r57_fast_decision(&w,&rom)==0);
 }
}
static void test_multiple_overlaps(void){
 static uint8_t rombytes[1048576];
 const Rom rom={rombytes,sizeof rombytes};
 const uint16_t a=W_ACTOR_SLOTS, b=a+ACTOR_SLOT_STRIDE, c=b+ACTOR_SLOT_STRIDE;
 for(unsigned t=0;t<9000;t++){
  memset(&w,0,sizeof w);
  wram_w16(&w,W_ACTOR_LIST_HEAD,(t%2)?a:c);
  if(t%2){put_rec(a,b,5,0);put_rec(b,c,6,2);put_rec(c,0,7,4);}
  else {put_rec(c,b,7,4);put_rec(b,a,6,2);put_rec(a,0,5,0);}
  write_entry(0,ACTOR_DEEB_COLLIDE_ENTRY);
  write_entry(2,ACTOR_DEEB_COLLIDE_ENTRY);
  write_entry(4,ACTOR_DEEB_COLLIDE_ENTRY);
  prior=w;
  assert(sprite_build_oam_r57_fast_decision(&w,&rom)==R68_APPROVE_READONLY_INPUT);
  assert(memcmp(&prior,&w,sizeof w)==0);
  // One pair now sends $00FF to a handler that can write/park: MUST defer.
  wram_w16(&w,c+ACTOR_COLLIDE_ID,DEEB_ID_STOP);
  assert(sprite_build_oam_r57_fast_decision(&w,&rom)==R52_DEFER_TO_SANDBOX);
  // Even with safe IDs, a referenced metasprite outside ROM must defer.
  wram_w16(&w,c+ACTOR_COLLIDE_ID,7);
  wram_w16(&w,a+ACTOR_META,0xffff);
  wram_w16(&w,a+ACTOR_META_BANK,SPRITE_META_BANK_HI);
  // At address $90:FFFF, available=1. Set count so sprites exceed it.
  const uint32_t off=(SPRITE_META_BANK_HI&0x7fu)*0x8000u+0x7fffu;
  rombytes[off]=255;
  assert(sprite_build_oam_r57_fast_decision(&w,&rom)==R52_DEFER_TO_SANDBOX);
  rombytes[off]=0;
 }
}
int main(void){test_callbacks();test_oam();test_multiple_overlaps();
 puts("R68 PASS: 36,000 randomized actual callback full-WRAM read-only proofs; 20,000 paired plus 9,000 three-actor cases (87,000 total OAM decisions)");}
