// R74: live callback execution vs the precise pair/OAM read-only fast verdict.
// Does not use the private game ROM. Original 65816 oracle remains separate.
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "port/r52_fast_guard.h"

static Wram w, prior;
static uint32_t seed=0x74c0111du;
static uint32_t rnd(void){seed ^= seed<<13; seed ^=seed>>17; seed ^=seed<<5;return seed;}
static void reg_set(uint16_t slot,uint32_t e){
  wram_w16(&w,W_THREAD_HANDLER+slot,(uint16_t)e);
  wram_w16(&w,W_THREAD_HANDLER_BANK+slot,(uint16_t)(e>>16));
}
static void actor(uint16_t at,uint16_t next,uint16_t id,uint16_t slot){
 wram_w16(&w,at+ACTOR_NEXT,next);
 wram_w16(&w,at+ACTOR_FLAGS,ACTOR_DRAW|ACTOR_SCREEN_SPACE);
 wram_w16(&w,at+ACTOR_X,120);wram_w16(&w,at+ACTOR_Y,120);
 wram_w16(&w,at+ACTOR_COLLIDE_ID,id);
 wram_w16(&w,at+ACTOR_THREAD,slot);
 wram_w16(&w,at+ACTOR_META,0);
}
int main(void){
 static uint8_t rom_data[1048576];
 const Rom rom={rom_data,sizeof rom_data};
 const uint16_t a=W_ACTOR_SLOTS,b=a+ACTOR_SLOT_STRIDE,c=b+ACTOR_SLOT_STRIDE;
 uint32_t single=0,total=0,readonly=0,oam=0,unsafe=0,negative=0;
 for(uint32_t i=0;i<24000;i++){
   for(unsigned n=0;n<WRAM_SIZE;n++) w.bytes[n]=(uint8_t)rnd();
   const uint16_t first_arg=(uint16_t)(rnd()%COLLIDE_ID_PLAYER);
   const uint16_t second_arg=(uint16_t)rnd();
   const uint16_t bs=(uint16_t)(2*(rnd()%24));
   const uint16_t as=(uint16_t)((bs+2u+(2u*(rnd()%23)))%48u);
   actor(a,b,first_arg,as);actor(b,0,second_arg,bs);
   // This handler (on INNER b) executes before OUTER a.
   reg_set(bs,ENEMY_9A6D_COLLIDE_ENTRY);
   reg_set(as,0);
   prior=w;
   assert(r58_notify_decision(&w,a,b)==R52_DEFER_TO_SANDBOX);
   assert(r74_notify_decision(&w,a,b)==R74_APPROVE_PAIR_READONLY);
   assert(!memcmp(&prior,&w,sizeof w));
   if ((i & 31u)==0) {
     Wram sandbox=w;
     ThreadCallResult tail={.c=(rnd()&1)!=0};
     assert(actor_collide_notify(&sandbox,&rom,a,b,&tail));
   }
   single++;
   // Use the actual ported callback, checking full 128KiB WRAM stability.
   ActorHandlerRegs r={(uint16_t)rnd(),(uint16_t)rnd(),first_arg,0,0,1};
   assert(enemy_d7f6_collide(&w,&rom,(uint16_t)rnd(),first_arg,&r,NULL));
   assert(!memcmp(&prior,&w,sizeof w));
   // Reset and test previously deferred total second callbacks: they can write.
   w=prior;reg_set(as,ACTOR_DEEB_COLLIDE_ENTRY);
   assert(r74_notify_decision(&w,a,b)==R74_APPROVE_PAIR_READONLY);
   if ((i & 31u)==0) {
     Wram sandbox=w;ThreadCallResult tail={.c=(rnd()&1)!=0};
     assert(actor_collide_notify(&sandbox,&rom,a,b,&tail));
   }
   total++;
   // Six read-only handler branches can safely run as the second callback.
   w=prior;reg_set(as,ACTOR_A638_COLLIDE_ENTRY);
   wram_w16(&w,b+ACTOR_COLLIDE_ID,0x0038);
   assert(r68_handler_readonly_for_arg(ACTOR_A638_COLLIDE_ENTRY,0x0038));
   if(r74_notify_decision(&w,a,b)!=R74_APPROVE_PAIR_READONLY){
      fprintf(stderr,"FAIL i=%u bs=%x as=%x first=%x second=%x old=%d got=%d\n",i,bs,as,first_arg,0x38,r58_notify_decision(&w,a,b),r74_notify_decision(&w,a,b));return 1;
   }
   if ((i & 31u)==0) {
     Wram sandbox=w;ThreadCallResult tail={.c=(rnd()&1)!=0};
     assert(actor_collide_notify(&sandbox,&rom,a,b,&tail));
   }
   readonly++;
   // A second unproved callback with an input that may mutate must defer.
   reg_set(as,ENEMY_9A6D_COLLIDE_ENTRY);
   wram_w16(&w,b+ACTOR_COLLIDE_ID,COLLIDE_ID_PLAYER);
   assert(r74_notify_decision(&w,a,b)==R52_DEFER_TO_SANDBOX);
   unsafe++;
   // If the FIRST 9A6D argument reaches a mutating branch, approval is forbidden.
   w=prior;wram_w16(&w,a+ACTOR_COLLIDE_ID,COLLIDE_ID_PLAYER);
   assert(r74_notify_decision(&w,a,b)==R52_DEFER_TO_SANDBOX);
   unsafe++;
   // Unsupported second callback can safely reject ONLY with read-only first.
   w=prior;reg_set(as,0x829fa1u);
   assert(r74_notify_decision(&w,a,b)==R52_REJECT_UNPORTED);
   negative++;
 }
 for(uint32_t t=0;t<14000;t++){
   memset(&w,0,sizeof w);
   wram_w16(&w,W_ACTOR_LIST_HEAD,a);
   const uint16_t aid=(uint16_t)(1u+(t%20));
   const uint16_t bid=(uint16_t)(30u+(t%20));
   actor(a,b,aid,0);actor(b,0,bid,2);
   reg_set(0,ENEMY_9A6D_COLLIDE_ENTRY);reg_set(2,0);
   prior=w;
   assert(sprite_build_oam_r57_fast_decision(&w,&rom)==R74_APPROVE_PAIR_READONLY);
   assert(!memcmp(&prior,&w,sizeof w));
   // Cross-check the actual legacy sort/cull/collision preflight on its
   // separate 128 KiB scratch image; the optimized predicate must imply pass.
   Wram scratch=prior;
   assert(sprite_build_oam_supported_preflight(&scratch,&rom,0));
   oam++;
   // Same handler with an unsafe incoming ID must take original sandbox.
   wram_w16(&w,b+ACTOR_COLLIDE_ID,COLLIDE_ID_PLAYER);
   assert(sprite_build_oam_r57_fast_decision(&w,&rom)==R52_DEFER_TO_SANDBOX);
   unsafe++;
   // Even if one pair is safe, a DIFFERENT third actor may send a mutating ID.
   wram_w16(&w,W_ACTOR_LIST_HEAD,a);
   actor(a,b,aid,0);actor(b,c,bid,2);actor(c,0,COLLIDE_ID_PLAYER,4);
   reg_set(4,0);
   assert(sprite_build_oam_r57_fast_decision(&w,&rom)==R52_DEFER_TO_SANDBOX);
   unsafe++;
   // Malformed cyclic lists always defer rather than incorrectly approve.
   wram_w16(&w,c+ACTOR_NEXT,a);
   assert(sprite_build_oam_r57_fast_decision(&w,&rom)==R52_DEFER_TO_SANDBOX);
   unsafe++;
 }
 printf("R74 PASS: %u read-only real callbacks, %u empty-second, %u total-second, %u readonly-second, %u OAM approvals, %u unsafe deferrals, %u unported second rejections\n",
   24000,single,total,readonly,oam,unsafe,negative);
 return 0;
}
