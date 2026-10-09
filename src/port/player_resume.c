#include "port/player_resume.h"

// The same register helpers used by the existing native thread bodies.
// Work is NULL on the R36 immediate-commit path: no fake cycle work there.
static void price(PlayerResumeWork* k, int cycles, int bytes, int dp) {
  if (k) { k->fast_cycles += cycles; k->rom_bytes += bytes; k->dp_accesses += dp; }
}
static uint16_t word(const Wram* w, const PortCpu* c, unsigned off) {
  return wram_r16(w, (uint16_t)(c->d + off));
}
static void load(PortCpu* c, uint16_t value) { c->a = value; set_nz16(c, value); }
static bool changed(const Wram* w, PortCpu* c, PlayerResumeWork* k) {
  load(c, word(w,c,0x1a)); cmp16(c,c->a,word(w,c,0x1c));
  price(k,68,6,2); // LDA dp, CMP dp, BNE
  if (!flag(c,PORT_P_Z)) {
    price(k,24,3,0); // taken branch + JMP $D4E9
    c->pc=0x80d4e9; return true;
  }
  return false;
}
void player_idle_resume(const Wram* w, PortCpu* c, PlayerResumeWork* k) {
  // $D53D..$D55A, complete idle dispatch including all three outgoing paths.
  if (changed(w,c,k)) return;
  load(c,word(w,c,0x4c)); price(k,40,4,1);
  if (c->a) { price(k,6,0,0); c->pc=0x80d557; return; }
  load(c,word(w,c,0x1e)); price(k,40,4,1);
  if (c->a) { price(k,6,0,0); c->pc=0x80d554; return; }
  load(c,(uint16_t)(word(w,c,0x20)|word(w,c,0x6c))); price(k,68,6,2);
  if (!c->a) { price(k,6,0,0); c->pc=0x80d557; }
  else { price(k,18,3,0); c->pc=0x80d4f4; }
}
void player_walk_resume(const Wram* w, PortCpu* c, PlayerResumeWork* k) {
  // $D6A8..$D6B2. The animation call/pose tail stays in the existing chain.
  if (changed(w,c,k)) return;
  load(c,word(w,c,0x16)); price(k,40,4,1);
  if (c->a) { price(k,6,0,0); c->pc=0x80d700; }
  else c->pc=0x80d6b2;
}
void player_walk_fire_resume(Wram* w, PortCpu* c, PlayerResumeWork* k) {
  // $D6B8..$D6D5, stopping at the shooting or animation JSR.
  if (changed(w,c,k)) return;
  load(c,word(w,c,0x16)); price(k,40,4,1);
  if (c->a) { price(k,6,0,0); c->pc=0x80d700; return; }
  load(c,(uint16_t)(word(w,c,0x4c)|word(w,c,0x18))); price(k,68,6,2);
  if (!c->a) { c->pc=0x80d6c8; return; }
  price(k,52,5,1); // taken BNE, LDA #$D75F, STA $14
  load(c,0xd75f); wram_w16(w,c->d+0x14,c->a); c->pc=0x80d6d5;
}
void player_walk_after_fire(Wram* w, PortCpu* c, PlayerResumeWork* k) {
  // $D6CB..$D6D5, resume after the external shooting call.
  load(c,0xd85f); wram_w16(w,c->d+0x14,c->a);
  price(k,64,7,1); c->pc=0x80d6d5;
}
void player_walk_animation(Wram* w, PortCpu* c, PlayerResumeWork* k) {
  // $D72A..$D74E: whole walk-animation helper, tail-jumping to pose setup.
  load(c,4); bit16(c,word(w,c,0x54)); price(k,58,7,1);
  if (!flag(c,PORT_P_N)) price(k,6,0,0);
  else {
    load(c,6); price(k,30,5,0);
    if (flag(c,PORT_P_V)) price(k,6,0,0);
    else { load(c,2); price(k,18,3,0); }
  }
  wram_w16(w,c->d+0x16,c->a);
  load(c,(uint16_t)((word(w,c,0x18)+1u)&3u));
  wram_w16(w,c->d+0x18,c->a);
  load(c,(uint16_t)(word(w,c,0x26)-2u));
  c->a=asl16(c,c->a);
  load(c,(uint16_t)(c->a|word(w,c,0x18)));
  c->a=asl16(c,c->a); c->a=asl16(c,c->a);
  price(k,248,22,5); c->pc=0x80f300;
}
