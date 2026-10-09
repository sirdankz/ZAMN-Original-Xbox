#include "port/player.h"
#include "port/apu.h"
#include "port/collide.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned sfx_count;
void apu_play_sfx(Wram* w, uint16_t id, uint16_t dp, ApuSfxRegs* out) {
  (void)w;(void)dp; assert(id==WEAPON_SFX_SWITCH);++sfx_count;
  if(out) memset(out,0,sizeof(*out));
}
static void romput(uint8_t* mem, uint32_t pc, uint16_t val) {
  uint32_t off=((pc>>16)&0x7f)*0x8000+(pc&0xffff)-0x8000;
  mem[off]=(uint8_t)val;mem[off+1]=(uint8_t)(val>>8);
}
static void setup(Wram* w,uint8_t* rommem,Rom* rom, uint16_t dp, unsigned player) {
  memset(w,0,sizeof(*w));memset(rommem,0,65536);
  rom->data=rommem;rom->size=65536;
  wram_w16(w,dp+ACTOR_DP_PLAYER,(uint16_t)(player*2));
  wram_w16(w,dp+PLAYER_DP_TABLE_INDEX,(uint16_t)(player*2));
  wram_w16(w,dp+ACTOR_DP_RECORD,(uint16_t)(0x1800+player*32));
  romput(rommem,PLAYER_INVENTORY_BASES+(uint32_t)player*2,(uint16_t)(0x1ccc+player*28));
  romput(rommem,WEAPON_DATA_TABLES+(uint32_t)player*2,0xfda0);
  for(int i=0;i<14;i++)romput(rommem,0x80fda0u+2u*i,(uint16_t)(0x200+i));
  // multiple owned weapons to make backwards differ from forwards
  for(int i=0;i<14;i++) wram_w16(w,0x1ccc+(uint32_t)player*28+2*i,(i==0||i==2||i==5)?1:0);
  wram_w16(w,W_PLAYER_WEAPON+player*2,5);
}
int main() {
 Wram *w=calloc(1,sizeof(Wram));uint8_t*rommem=calloc(1,65536);Rom rom;
 assert(w && rommem);
 for(unsigned p=0;p<2;p++) {
  const uint16_t dp=(uint16_t)(0x100+0x80*p);
  setup(w,rommem,&rom,dp,p);
  player_cycle_clear();sfx_count=0;
  player_cycle_request((uint16_t)(p*2),PSN_CYCLE_WEAPON,-1);
  assert(player_cycle_pending(p*2,PSN_CYCLE_WEAPON)==-1);
  // Common dispatch hook runs whether next body chooses translated or ROM path.
  player_cycle_at_state_entry(w,&rom,dp);
  assert(wram_r16(w,W_PLAYER_WEAPON+p*2)==2);
  assert(player_cycle_pending(p*2,PSN_CYCLE_WEAPON)==0);
  assert(sfx_count==1);
  player_cycle_at_state_entry(w,&rom,dp);
  assert(sfx_count==1); // at most one change per trigger edge
  player_cycle_request((uint16_t)(p*2),PSN_CYCLE_WEAPON,-1);
  player_cycle_at_state_entry(w,&rom,dp);
  assert(wram_r16(w,W_PLAYER_WEAPON+p*2)==0);
  player_cycle_request((uint16_t)(p*2),PSN_CYCLE_WEAPON,-1);
  player_cycle_at_state_entry(w,&rom,dp);
  assert(wram_r16(w,W_PLAYER_WEAPON+p*2)==5); // wraps backwards
  // No request from other players, malformed DP, or forward-only request
  player_cycle_request((uint16_t)(p*2),PSN_CYCLE_WEAPON,+1);
  player_cycle_at_state_entry(w,&rom,dp);
  assert(wram_r16(w,W_PLAYER_WEAPON+p*2)==5);
  assert(player_cycle_pending(p*2,PSN_CYCLE_WEAPON)==1);
  player_cycle_clear();
  player_cycle_request((uint16_t)(p*2),PSN_CYCLE_WEAPON,-1);
  player_cycle_at_state_entry(w,&rom,0x1f80); // invalid page rejected
  assert(player_cycle_pending(p*2,PSN_CYCLE_WEAPON)==-1);
 }
 free(w);free(rommem);
 puts("R76 common native/fallback entry: reverse walk, wrap, single-press, per-player, invalid-DP PASS");
 return 0;
}
