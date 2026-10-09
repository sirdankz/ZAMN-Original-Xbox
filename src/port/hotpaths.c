#include "port/hotpaths.h"

static void price(NativeHotWork* k, int cycles, int bytes) {
  if (k) { k->fast_cycles += cycles; k->rom_bytes += bytes; }
}
static void lda(PortCpu* c, uint16_t a) { c->a=a; set_nz16(c,a); }
static void ldx(PortCpu* c, uint16_t x) { c->x=x; set_nz16(c,x); }
static void ldy(PortCpu* c, uint16_t y) { c->y=y; set_nz16(c,y); }
static bool common(const PortCpu* c) {
  return c->d==0 && c->db==0x80 && !(c->p & (PORT_P_M|PORT_P_X|PORT_P_D)) &&
         c->s>=0x100 && c->s<0x2000;
}
bool native_overlap_supported(const Wram* w, const PortCpu* c) {
  if (!common(c)) return false;
  uint16_t top;
  if (c->pc==0x80bec9u) {
    uint16_t n=wram_r16(w,0x9c);
    if ((n&1) || n>64) return false;
    if (n<=2) return true;
    top=(uint16_t)(n-2);
  } else if (c->pc==0x80bf12u) {
    uint16_t outer=wram_r16(w,0x3c);
    if ((c->y&1) || c->y>=64 || (outer&1) || outer>=64) return false;
    top=c->y>outer?c->y:outer;
  } else return false;
  // Only actual actor slots: every DP,X read stays in low WRAM and cannot
  // alias the scan's scratch or list. Recheck after each external handler.
  for (unsigned i=0;i<=top;i+=2) {
    uint16_t a=wram_r16(w,0x137e + i);
    if (a<0x185e || a>0x1aca || (a-0x185e)%0x14) return false;
  }
  return true;
}
void native_overlap_scan(Wram* w, PortCpu* c, NativeHotWork* k) {
  if (c->pc==0x80bf12u) goto next_inner;
  ldy(c,wram_r16(w,0x9c)); price(k,28,2);
  price(k,c->y?12:18,2); if (!c->y) goto done;
  ldy(c,(uint16_t)(c->y-2)); price(k,24,2);
  price(k,c->y?12:18,2); if (!c->y) goto done;
outer:
  ldx(c,wram_r16(w,0x137e + c->y));
  ldy(c,(uint16_t)(c->y-2)); wram_w16(w,0x3c,c->y);
  lda(c,wram_r16(w,c->x+0x0e)); price(k,126,9);
  price(k,c->a?12:18,2); if (!c->a) goto next_outer;
  wram_w16(w,0x4a,c->a);
  lda(c,wram_r16(w,c->x+2)); wram_w16(w,0x38,c->a);
  lda(c,wram_r16(w,c->x+6)); wram_w16(w,0x3a,c->a); price(k,152,10);
inner:
  ldx(c,wram_r16(w,0x137e + c->y)); lda(c,wram_r16(w,c->x+0x0e)); price(k,74,5);
  price(k,c->a?12:18,2); if (!c->a) goto next_inner;
  cmp16(c,c->a,wram_r16(w,0x4a)); price(k,28,2);
  price(k,flag(c,PORT_P_Z)?18:12,2); if (flag(c,PORT_P_Z)) goto next_inner;
  lda(c,wram_r16(w,c->x+2)); set_c(c,true);
  c->a=sbc16(c,c->a,wram_r16(w,0x38)); set_c(c,false);
  c->a=adc16(c,c->a,8); cmp16(c,c->a,16); price(k,122,12);
  price(k,flag(c,PORT_P_C)?18:12,2); if (flag(c,PORT_P_C)) goto next_inner;
  lda(c,wram_r16(w,c->x+6)); set_c(c,true);
  c->a=sbc16(c,c->a,wram_r16(w,0x3a)); set_c(c,false);
  c->a=adc16(c,c->a,8); cmp16(c,c->a,16); price(k,122,12);
  price(k,flag(c,PORT_P_C)?18:12,2); if (flag(c,PORT_P_C)) goto next_inner;
  // Leave PHY/JSR/PLY and every collision side effect to the existing handler
  // path. Nothing is cached across it, including scratch written by handlers.
  c->pc=0x80bf0du; return;
next_inner:
  ldy(c,(uint16_t)(c->y-2)); price(k,24,2);
  price(k,flag(c,PORT_P_N)?12:18,2); if (!flag(c,PORT_P_N)) goto inner;
next_outer:
  ldy(c,wram_r16(w,0x3c)); price(k,28,2);
  price(k,c->y?18:12,2); if (c->y) goto outer;
done:
  c->pc=0x80bf1au; // RTL itself still runs, with the original stack and flags.
}

bool native_sprite_supported(const Wram* w, const Rom* rom, const PortCpu* c) {
  if (!common(c)) return false;
  bool entry = false;
  switch (c->pc) {
    case 0x80ba51u: case 0x80babau: case 0x80bb30u: case 0x80bba6u:
      entry = true; break;
    case 0x80ba9bu: case 0x80bb0bu: case 0x80bb81u: case 0x80bbfeu:
      break;
    default: return false;
  }
  uint16_t x=entry?wram_r16(w,0x88):c->x;
  uint16_t n=wram_r16(w,0x86), ptr=wram_r16(w,0x8a);
  uint8_t bank=wram_r8(w,0x8c);
  if (x>=0x200 || (x&3) || !n || n>128 || (bank!=0x8f && bank!=0x90)) return false;
  return rom_has(rom,((uint32_t)bank<<16)|ptr,(uint32_t)n*8) &&
         rom_has(rom,0x80b747,0x200);
}
static uint16_t piece(const Wram* w, const Rom* rom, unsigned off) {
  uint32_t ptr=((uint32_t)wram_r8(w,0x8c)<<16)|wram_r16(w,0x8a);
  return rom_word(rom,ptr+off);
}
static unsigned sprite_variant(uint32_t pc, bool* resume) {
  *resume=false;
  switch (pc) {
    case 0x80ba51u: return 0; // no flip
    case 0x80babau: return 2; // horizontal
    case 0x80bb30u: return 4; // vertical
    case 0x80bba6u: return 6; // horizontal + vertical
    case 0x80ba9bu: *resume=true; return 0;
    case 0x80bb0bu: *resume=true; return 2;
    case 0x80bb81u: *resume=true; return 4;
    case 0x80bbfeu: *resume=true; return 6;
    default: return 0;
  }
}
static uint32_t sprite_call_pc(unsigned flip) {
  static const uint32_t pc[4]={0x80ba98u,0x80bb08u,0x80bb7eu,0x80bbfbu};
  return pc[(flip&6)>>1];
}
static uint32_t sprite_return_pc(unsigned flip) {
  static const uint32_t pc[4]={0x80bab9u,0x80bb2fu,0x80bba5u,0x80bc22u};
  return pc[(flip&6)>>1];
}
void native_sprite_emit(Wram* w, const Rom* rom, PortCpu* c, NativeHotWork* k) {
  bool resume=false;
  const unsigned flip=sprite_variant(c->pc,&resume);
  const bool fx=(flip&2)!=0, fy=(flip&4)!=0;
  if (resume) goto after_tile;
  ldx(c,wram_r16(w,0x88)); price(k,28,2);
next_piece:
  ldy(c,2); lda(c,piece(w,rom,2));
  if (fy) {
    lda(c,(uint16_t)(c->a^0xffffu)); set_c(c,true);
    c->a=sbc16(c,c->a,0x000f); price(k,48,7);
  }
  set_c(c,false); c->a=adc16(c,c->a,wram_r16(w,0x90));
  wram_w16(w,0x13bf+c->x,c->a); cmp16(c,c->a,0xfff1); price(k,164,16);
  if (flag(c,PORT_P_C)) { price(k,18,2); }
  else {
    price(k,12,2); cmp16(c,c->a,0xe0); price(k,18,3);
    price(k,flag(c,PORT_P_C)?18:12,2); if (flag(c,PORT_P_C)) goto advance;
  }
  lda(c,piece(w,rom,0));
  if (fx) {
    lda(c,(uint16_t)(c->a^0xffffu)); set_c(c,true);
    c->a=sbc16(c,c->a,0x000f); price(k,48,7);
  }
  set_c(c,false); c->a=adc16(c,c->a,wram_r16(w,0x8e));
  // SEP/STA/REP changes no flags; the 8-bit store leaves A's high byte intact.
  wram_w8(w,0x13be + c->x,(uint8_t)c->a); cmp16(c,c->a,0x100); price(k,174,17);
  if (!flag(c,PORT_P_C)) { price(k,18,2); }
  else {
    price(k,12,2); cmp16(c,c->a,0xfff1); price(k,18,3);
    price(k,flag(c,PORT_P_C)?12:18,2); if (!flag(c,PORT_P_C)) goto advance;
    ldy(c,rom_word(rom,0x80b747+c->x)); lda(c,rom_word(rom,0x80b749+c->x));
    if (flip==6) lda(c,(uint16_t)(c->a^wram_r16(w,0x13be + c->y)));
    else lda(c,(uint16_t)(c->a|wram_r16(w,0x13be + c->y)));
    wram_w16(w,0x13be + c->y,c->a); price(k,152,16);
  }
  ldy(c,4); lda(c,piece(w,rom,4)); lda(c,(uint16_t)(c->a&wram_r16(w,0x96)));
  wram_w16(w,0x94,c->a); ldy(c,6); lda(c,piece(w,rom,6)); price(k,188,18);
  c->pc=sprite_call_pc(flip); return; // Existing sprite_frame_tile call; resume at the variant's post-JSR PC.
after_tile:
  lda(c,(uint16_t)(c->a|wram_r16(w,0x94))); lda(c,(uint16_t)(c->a|wram_r16(w,0x92)));
  if (flip) { lda(c,(uint16_t)(c->a ^ (flip==2?0x4000u:flip==4?0x8000u:0xc000u))); price(k,18,3); }
  wram_w16(w,0x13c0+c->x,c->a); ldx(c,(uint16_t)(c->x+4)); cmp16(c,c->x,0x200);
  price(k,162,14); price(k,flag(c,PORT_P_Z)?18:12,2);
  if (flag(c,PORT_P_Z)) goto done;
advance:
  lda(c,wram_r16(w,0x8a)); set_c(c,false); c->a=adc16(c,c->a,8); wram_w16(w,0x8a,c->a);
  { uint16_t n=(uint16_t)(wram_r16(w,0x86)-1); wram_w16(w,0x86,n); set_nz16(c,n); }
  price(k,136,10);
  if (flip) {
    // Flipped emitters use BEQ + JMP because the larger body no longer fits a
    // relative loop-back.  The flags are still DEC $86's on both paths.
    price(k,flag(c,PORT_P_Z)?18:12,2);
    if (flag(c,PORT_P_Z)) goto done;
    price(k,18,3); goto next_piece;
  } else {
    price(k,flag(c,PORT_P_Z)?12:18,2);
    if (!flag(c,PORT_P_Z)) goto next_piece;
  }
done:
  wram_w16(w,0x88,c->x); price(k,28,2); c->pc=sprite_return_pc(flip);
}


// R51: native slices through sprite_build_oam's record walker.  The complete
// sprite_build_oam substitution remains preferred at $80:BD1F.  These entries
// are reached only when its conservative guard declines, so the ROM still owns
// sort/cull/clear, the indirect emitter call itself, and the overlap JSL.  The
// slices remove the repeated 65816 bookkeeping between those already-native
// boundaries without speculating across collision side effects.
static bool actor_list_ok(const Wram* w, const Rom* rom) {
  uint16_t n=wram_r16(w,0x9c);
  if ((n&1u) || n>64u) return false;
  for (uint16_t i=0;i<n;i=(uint16_t)(i+2)) {
    uint16_t rec=wram_r16(w,0x137e + i);
    if (rec<0x185e || rec>0x1aca || ((rec-0x185e)%0x14)!=0) return false;
    uint16_t flags=wram_r16(w,rec);
    if (!(flags&0x8000u)) continue; // ACTOR_DRAW
    uint16_t ptr=wram_r16(w,(uint32_t)rec+8);
    uint16_t bank=wram_r16(w,(uint32_t)rec+0x0a);
    if (ptr>=0x8000u && bank>=0x008fu && bank<0x0091u &&
        !rom_has(rom,((uint32_t)(bank&0xffu)<<16)|ptr,2)) return false;
  }
  return true;
}

bool native_oam_walk_supported(const Wram* w, const Rom* rom, const PortCpu* c) {
  if (!common(c)) return false;
  if (c->pc==0x80bd30u) {
    uint16_t oi=wram_r16(w,0x88);
    return oi<=0x0200u && !(oi&3u) && actor_list_ok(w,rom);
  }
  if (c->pc==0x80bdb7u) {
    uint16_t oi=wram_r16(w,0x88);
    if (c->x>0x0200u || (c->x&3u) || oi!=c->x || !actor_list_ok(w,rom)) return false;
    if (c->x==0x0200u) return true;
    uint16_t n=wram_r16(w,0x9c), cur=wram_r16(w,0x9a);
    return n>=2u && !(cur&1u) && cur<n;
  }
  if (c->pc==0x80bdd0u) {
    if (c->s>0x1ffcu) return false;
    uint16_t d=(uint16_t)(wram_r8(w,(uint16_t)(c->s+1)) |
                          ((uint16_t)wram_r8(w,(uint16_t)(c->s+2))<<8));
    uint8_t db=wram_r8(w,(uint16_t)(c->s+3));
    uint16_t dp20=(uint16_t)(d+0x20u);
    if (dp20<d || dp20>=0x2000u) return false;
    if (!(db<0x40u || (db>=0x80u && db<0xc0u))) return false;
    return rom_has(rom,((uint32_t)db<<16)|0xbde6u,4);
  }
  return false;
}

static void and16(PortCpu* c,uint16_t v){ lda(c,(uint16_t)(c->a&v)); }
static void ora16(PortCpu* c,uint16_t v){ lda(c,(uint16_t)(c->a|v)); }

void native_oam_walk(Wram* w, const Rom* rom, PortCpu* c, NativeHotWork* k) {
  (void)k; // Release-native R51 slice: diagnostic builds leave this slice off.
  if (c->pc==0x80bdd0u) goto epilogue;
  if (c->pc==0x80bdb7u) goto after_emit;

  // $80:BD30..: the three setup calls have already returned with D=0/DB=$80.
  lda(c,wram_r16(w,0x20)); wram_w16(w,0xa0,c->a);
  lda(c,wram_r16(w,0x9c));
  if (flag(c,PORT_P_Z)) goto overlap_call;
  wram_w16(w,0x88,0); // STZ does not alter flags.
  ldy(c,0);

walk_record:
  wram_w16(w,0x9a,c->y);
  ldx(c,wram_r16(w,0x137e + c->y));
  lda(c,wram_r16(w,c->x));
  if (!flag(c,PORT_P_N)) goto after_emit;

  ldy(c,0x2000); and16(c,0x0008);
  if (!flag(c,PORT_P_Z)) ldy(c,0x3000);
  wram_w16(w,0x92,c->y);
  ldy(c,0xffff);
  lda(c,wram_r16(w,c->x)); and16(c,0x0010);
  if (!flag(c,PORT_P_Z)) {
    lda(c,wram_r16(w,(uint32_t)c->x+0x10)); ora16(c,wram_r16(w,0x92));
    wram_w16(w,0x92,c->a); ldy(c,0xf1ff);
  }
  wram_w16(w,0x96,c->y);

  lda(c,wram_r16(w,c->x)); c->a=asl16(c,c->a);
  if (flag(c,PORT_P_N)) {
    lda(c,wram_r16(w,(uint32_t)c->x+2)); wram_w16(w,0x8e,c->a);
    lda(c,wram_r16(w,(uint32_t)c->x+6)); wram_w16(w,0x90,c->a);
  } else {
    lda(c,wram_r16(w,(uint32_t)c->x+2)); set_c(c,true);
    c->a=sbc16(c,c->a,wram_r16(w,0x1b6a)); wram_w16(w,0x8e,c->a);
    lda(c,wram_r16(w,(uint32_t)c->x+6)); set_c(c,true);
    c->a=sbc16(c,c->a,wram_r16(w,(uint32_t)c->x+4)); set_c(c,true);
    c->a=sbc16(c,c->a,wram_r16(w,0x1b6c)); wram_w16(w,0x90,c->a);
  }

  lda(c,wram_r16(w,(uint32_t)c->x+8)); cmp16(c,c->a,0x8000);
  if (!flag(c,PORT_P_C)) goto after_emit;
  wram_w16(w,0x8a,c->a);
  lda(c,wram_r16(w,(uint32_t)c->x+0x0a)); cmp16(c,c->a,0x008f);
  if (!flag(c,PORT_P_C)) goto after_emit;
  cmp16(c,c->a,0x0091);
  if (flag(c,PORT_P_C)) goto after_emit;
  wram_w16(w,0x8c,c->a);
  {
    uint32_t ptr=((uint32_t)wram_r8(w,0x8c)<<16)|wram_r16(w,0x8a);
    lda(c,bus_r16(w,rom,ptr)); and16(c,0x00ff); wram_w16(w,0x86,c->a);
  }
  if (flag(c,PORT_P_Z)) goto after_emit;
  {
    uint16_t v=(uint16_t)(wram_r16(w,0x8a)+1);
    wram_w16(w,0x8a,v); set_nz16(c,v); // INC $8A
  }
  lda(c,wram_r16(w,c->x)); and16(c,0x0006); ldx(c,c->a);
  c->pc=0x80bdb4u; // Let the ROM execute JSR ($BDEA,X); emitter entries are native.
  return;

after_emit:
  cmp16(c,c->x,0x0200);
  if (flag(c,PORT_P_Z)) goto overlap_call;
  ldy(c,wram_r16(w,0x9a)); ldy(c,(uint16_t)(c->y+1)); ldy(c,(uint16_t)(c->y+1));
  cmp16(c,c->y,wram_r16(w,0x9c));
  if (!flag(c,PORT_P_Z)) goto walk_record;
  ldx(c,wram_r16(w,0x88)); lda(c,0xe000); wram_w16(w,0x13be + c->x,c->a);

overlap_call:
  c->pc=0x80bdccu; // Existing JSL $80:BEC9; resume at $80:BDD0.
  return;

epilogue:
  c->d=pull16(w,c); set_nz16(c,c->d); // PLD
  c->db=pull8(w,c); set_nz8(c,c->db);  // PLB
  lda(c,wram_r16(w,(uint16_t)(c->d+0x20u))); and16(c,0x0003); ldx(c,c->a);
  lda(c,bus_r16(w,rom,((uint32_t)c->db<<16)|(uint16_t)(0xbde6u+c->x)));
  and16(c,0x00ff); wram_w16(w,0x1b64,c->a); set_c(c,true);
  c->pc=0x80bde2u; // RTL itself stays with the core.
}

// R53: original ROM $82:9715-$82:99D8, non-call movement/actor slices.
// Boundaries stop before JSL / JSR or at a return opcode. No handler is
// executed here; normal dispatcher/ROM executes every external side effect.
static uint16_t r53_dp(const PortCpu* c, uint16_t off) {
  return (uint16_t)(c->d + off);
}
static uint16_t r53_get(const Wram* w, const PortCpu* c, uint16_t off) {
  return wram_r16(w,r53_dp(c,off));
}
static void r53_put(Wram* w, const PortCpu* c, uint16_t off, uint16_t v) {
  wram_w16(w,r53_dp(c,off),v);
}
static void r53_and(PortCpu* c, uint16_t v) { lda(c,(uint16_t)(c->a & v)); }
static void r53_ora(PortCpu* c, uint16_t v) { lda(c,(uint16_t)(c->a | v)); }
static void r53_inca(PortCpu* c) { lda(c,(uint16_t)(c->a+1)); }
static void r53_deca(PortCpu* c) { lda(c,(uint16_t)(c->a-1)); }
static void r53_asla(PortCpu* c) { c->a=asl16(c,c->a); }
static uint16_t r53_table(const Wram* w,const Rom* rom,const PortCpu* c,uint16_t off) {
  return bus_r16(w,rom,((uint32_t)c->db<<16)|off);
}
// These 24 entry PCs start at instruction boundaries proved from the ROM.
// Reject any unknown entry or CPU mode instead of speculating about 8-bit
// accumulator/index, decimal arithmetic, or non-WRAM direct page.
static bool r53_entry(uint32_t pc) {
  switch (pc) {
    case 0x829715: case 0x82973e: case 0x82974a:
    case 0x829751: case 0x829759: case 0x829765:
    case 0x829773: case 0x82977f: case 0x8297be:
    case 0x8297d4: case 0x8297e0: case 0x8297e9:
    case 0x829800: case 0x82980e: case 0x82981a:
    case 0x829831: case 0x82983f: case 0x82985b:
    case 0x829863: case 0x82986b: case 0x8298ea:
    case 0x8298fe: case 0x829994: case 0x8299cb:
      return true;
    default: return false;
  }
}
bool native_r53_supported(const Wram* w,const Rom* rom,const PortCpu* c) {
  // Low 8KiB is WRAM-mirrored only in banks $00-$3F and $80-$BF.
  if (!r53_entry(c->pc) || c->d>0x1f00 ||
      !(c->db<0x40 || (c->db>=0x80 && c->db<0xc0)) ||
      (c->p&(PORT_P_M|PORT_P_X|PORT_P_D)) ||
      c->s < 0x100 || c->s>=0x2000) return false;
  // Indexed direction tables must not alias unrelated data. The real movement
  // state uses a small direction index. A corrupted state stays in ROM.
  if (c->pc==0x829715) {
    uint16_t index=(uint16_t)(((r53_get(w,c,0x16)-6)&15)+2);
    if (index>18) return false;
  }
  if (c->pc==0x8297e9 || c->pc==0x82981a) {
    if (r53_get(w,c,0x16)>0x0010) return false;
  }
  if (c->pc==0x82986b) {
    if (c->a>0x000f) return false;
  }
  if (c->pc==0x829994) {
    const uint16_t r=r53_get(w,c,0x08);
    // Only the actual actor table can be written by this native slice.
    if (r<0x185e || r>0x1aca || (r-0x185e)%0x14) return false;
    uint16_t direct=(uint16_t)((r53_get(w,c,0x16)<<1) | r53_get(w,c,0x1e));
    if (direct>0x0030 || !rom_has(rom,((uint32_t)c->db<<16) | (uint16_t)(0x99f3u+(uint16_t)(direct<<1)),2)) return false;
  }
  if (c->pc==0x8299cb) {
    const uint16_t r=r53_get(w,c,0x08);
    if (r<0x185e || r>0x1aca || (r-0x185e)%0x14) return false;
  }
  if (!rom_has(rom,((uint32_t)c->db<<16)|0x96d9u,0x40) ||
      !rom_has(rom,((uint32_t)c->db<<16)|0x990bu,4)) return false;
  return true;
}
void native_r53_movement(Wram* w,const Rom* rom,PortCpu* c,NativeHotWork* work) {
  (void)work; // No emulated-cycle pricing in the Release native-cutover.
  switch(c->pc) {
    case 0x829715: { // Rotate direction, calculate prospective position.
      lda(c,r53_get(w,c,0x16)); r53_deca(c);r53_deca(c);
      set_c(c,true); c->a=sbc16(c,c->a,4);r53_and(c,15);
      r53_inca(c);r53_inca(c);r53_put(w,c,0x18,c->a);
      r53_asla(c);ldx(c,c->a);
      lda(c,r53_table(w,rom,c,(uint16_t)(0x96d9+c->x)));
      set_c(c,false);c->a=adc16(c,c->a,r53_get(w,c,0x0e));r53_put(w,c,0x12,c->a);
      lda(c,r53_table(w,rom,c,(uint16_t)(0x96db+c->x)));
      set_c(c,false);c->a=adc16(c,c->a,r53_get(w,c,0x10));r53_put(w,c,0x14,c->a);
      ldx(c,r53_get(w,c,0x12));ldy(c,r53_get(w,c,0x14));
      c->pc=0x82973a;break;
    }
    case 0x82973e:
      if (flag(c,PORT_P_C)) c->pc=0x829750;
      else { lda(c,r53_get(w,c,0x08)); ldx(c,r53_get(w,c,0x12));
             ldy(c,r53_get(w,c,0x14)); c->pc=0x829746; }
      break;
    case 0x82974a:
      if (!flag(c,PORT_P_C)) { lda(c,r53_get(w,c,0x18));r53_put(w,c,0x16,c->a); }
      c->pc=0x829750;break;
    case 0x829751:
      ldx(c,r53_get(w,c,0x12));ldy(c,r53_get(w,c,0x10));c->pc=0x829755;break;
    case 0x829759:
      if (!flag(c,PORT_P_C)) {lda(c,r53_get(w,c,0x08));
        ldx(c,r53_get(w,c,0x12));ldy(c,r53_get(w,c,0x10));c->pc=0x829761; }
      else {ldx(c,r53_get(w,c,0x0e));ldy(c,r53_get(w,c,0x14));c->pc=0x82976f;}
      break;
    case 0x829765:
      if (!flag(c,PORT_P_C)) {lda(c,r53_get(w,c,0x12));r53_put(w,c,0x0e,c->a);}
      ldx(c,r53_get(w,c,0x0e));ldy(c,r53_get(w,c,0x14));c->pc=0x82976f;break;
    case 0x829773:
      if (flag(c,PORT_P_C)) c->pc=0x829785;
      else {lda(c,r53_get(w,c,0x08));ldx(c,r53_get(w,c,0x0e));
            ldy(c,r53_get(w,c,0x14));c->pc=0x82977b;}
      break;
    case 0x82977f:
      if (!flag(c,PORT_P_C)) {lda(c,r53_get(w,c,0x14));r53_put(w,c,0x10,c->a);}
      c->pc=0x829785;break;
    case 0x8297be:
      lda(c,r53_get(w,c,0x16));r53_deca(c);r53_deca(c);
      set_c(c,false);c->a=adc16(c,c->a,4);r53_and(c,15);
      r53_inca(c);r53_inca(c);r53_put(w,c,0x16,c->a);
      c->pc=0x82980e;break; // ROM JMP $980E
    case 0x8297d4:
      r53_and(c,3);r53_asla(c);r53_asla(c);r53_inca(c);r53_inca(c);
      r53_put(w,c,0x16,c->a);c->pc=0x8297e0;break;
    case 0x8297e0:
      lda(c,0x97e6);r53_put(w,c,0x1a,c->a);c->pc=0x8297e5;break;
    case 0x8297e9: case 0x82981a:
      lda(c,r53_get(w,c,0x16));r53_asla(c);ldx(c,c->a);
      lda(c,r53_table(w,rom,c,(uint16_t)(0x96d9+c->x)));
      set_c(c,false);c->a=adc16(c,c->a,r53_get(w,c,0x0e));r53_put(w,c,0x12,c->a);
      lda(c,r53_table(w,rom,c,(uint16_t)(0x96db+c->x)));
      set_c(c,false);c->a=adc16(c,c->a,r53_get(w,c,0x10));r53_put(w,c,0x14,c->a);
      c->pc=(c->pc==0x8297e9)?0x8297fd:0x82982e;break;
    case 0x829800: case 0x829831: {
      const bool later=c->pc==0x829831;
      if (flag(c,PORT_P_C)) c->pc=later?0x82983c:0x82980b;
      else {lda(c,r53_get(w,c,0x12));r53_put(w,c,0x0e,c->a);
            lda(c,r53_get(w,c,0x14));r53_put(w,c,0x10,c->a);
            c->pc=later?0x82983b:0x82980a;}
      break;
    }
    case 0x82980e:
      lda(c,0x9814);r53_put(w,c,0x1a,c->a);c->pc=0x829813;break;
    case 0x82983f:
      lda(c,0x9845);r53_put(w,c,0x1a,c->a);c->pc=0x829844;break;
    case 0x82985b:
      r53_and(c,2);c->pc=flag(c,PORT_P_Z)?0x829860:0x829863;break;
    case 0x829863:
      ldx(c,r53_get(w,c,0x08));ldy(c,r53_get(w,c,0x22));c->pc=0x829867;break;
    case 0x82986b:
      r53_asla(c);r53_put(w,c,0x16,c->a);r53_asla(c);ldx(c,c->a);
      lda(c,r53_table(w,rom,c,(uint16_t)(0x96d9+c->x)));
      set_c(c,false);c->a=adc16(c,c->a,r53_get(w,c,0x0e));r53_put(w,c,0x12,c->a);
      lda(c,r53_table(w,rom,c,(uint16_t)(0x96db+c->x)));
      set_c(c,false);c->a=adc16(c,c->a,r53_get(w,c,0x10));r53_put(w,c,0x14,c->a);
      c->pc=0x829880;break;
    case 0x8298ea:
      r53_and(c,2);ldx(c,c->a);
      lda(c,r53_table(w,rom,c,(uint16_t)(0x990b+c->x)));
      set_c(c,false);c->a=adc16(c,c->a,wram_r16(w,0x1b6a));
      r53_put(w,c,0x00,c->a);ldx(c,c->a);ldy(c,r53_get(w,c,0x02));
      c->pc=0x8298fa;break;
    case 0x8298fe:
      if (flag(c,PORT_P_C)) c->pc=0x82990a;
      else {ldx(c,r53_get(w,c,0x00));ldy(c,r53_get(w,c,0x02));c->pc=0x829904;}
      break;
    case 0x829994: {
      uint16_t tick=(uint16_t)(r53_get(w,c,0x1c)-1);
      r53_put(w,c,0x1c,tick);set_nz16(c,tick);
      if (!flag(c,PORT_P_N)) {c->pc=0x8299cb;break;}
      lda(c,2);r53_put(w,c,0x1c,c->a);
      lda(c,r53_get(w,c,0x16));r53_asla(c);r53_ora(c,r53_get(w,c,0x1e));
      r53_asla(c);ldy(c,c->a);ldx(c,r53_get(w,c,0x08));
      lda(c,r53_table(w,rom,c,(uint16_t)(0x99f3+c->y)));
      wram_w16(w,(uint16_t)(c->x+8),c->a);
      lda(c,wram_r16(w,c->x));cmp16(c,c->y,0x30);
      if (flag(c,PORT_P_C))r53_ora(c,2);
      else r53_and(c,0xfffd);
      wram_w16(w,c->x,c->a);
      lda(c,r53_get(w,c,0x1e));r53_inca(c);r53_and(c,3);r53_put(w,c,0x1e,c->a);
      lda(c,r53_get(w,c,0x1e));c->pc=flag(c,PORT_P_Z)?0x8299d8:0x8299cb;
      break;
    }
    case 0x8299cb:
      ldy(c,r53_get(w,c,0x08));lda(c,r53_get(w,c,0x0e));
      wram_w16(w,(uint16_t)(c->y+2),c->a);
      lda(c,r53_get(w,c,0x10));wram_w16(w,(uint16_t)(c->y+6),c->a);
      c->pc=0x8299d7;break;
    default: break;
  }
}


// R54: bound original $82:D8D9-$82:D9F5 actor/position-state pieces.
// Every case stops at an original call, branch, or loop boundary; none executes
// an external engine helper on the native side. This is C translation, not an
// instruction interpreter. The ROM fallback owns unsupported CPU/state modes.
static uint16_t r54_get(const Wram* w,const PortCpu* c,uint16_t o) {
  return wram_r16(w,(uint16_t)(c->d+o));
}
static void r54_put(Wram* w,const PortCpu* c,uint16_t o,uint16_t v) {
  wram_w16(w,(uint16_t)(c->d+o),v);
}
static void r54_lsr(PortCpu* c) {
  set_c(c,(c->a&1u)!=0); lda(c,(uint16_t)(c->a>>1));
}
static void r54_dec_dp(Wram* w,PortCpu* c,uint16_t o) {
  const uint16_t a=(uint16_t)(r54_get(w,c,o)-1);
  r54_put(w,c,o,a); set_nz16(c,a);
}
static void r54_inc_dp(Wram* w,PortCpu* c,uint16_t o) {
  const uint16_t a=(uint16_t)(r54_get(w,c,o)+1);
  r54_put(w,c,o,a); set_nz16(c,a);
}
static bool r54_entry(uint32_t pc) {
  switch(pc) {
    case 0x82d8db: case 0x82d8fd: case 0x82d907: case 0x82d913:
    case 0x82d91d: case 0x82d92a: case 0x82d938: case 0x82d945:
    case 0x82d94d: case 0x82d96e: case 0x82d989: case 0x82d999:
    case 0x82d9ae: case 0x82d9b4: case 0x82d9c3: case 0x82d9d5:
    case 0x82d9dc: case 0x82d9eb: return true;
    default: return false;
  }
}
bool native_r54_supported(const Wram* w,const Rom* rom,const PortCpu* c) {
  if (!r54_entry(c->pc) || !rom_has(rom,0x82d8db,0x120) ||
      c->d>0x1e00 || (c->p&(PORT_P_M|PORT_P_X|PORT_P_D)) ||
      !(c->db<0x40 || (c->db>=0x80 && c->db<0xc0)) ||
      c->s<0x100 || c->s>=0x2000) return false;
  switch(c->pc) {
    case 0x82d8db: return r54_get(w,c,0)<=32;  // $1F98+(A*2) stays mirrored.
    case 0x82d8fd: return r54_get(w,c,0x1c)<=0x60;
    case 0x82d91d: case 0x82d999: return r54_get(w,c,0x08)<=0x1ff8;
    case 0x82d9ae: return c->y<=0x1ff8;
    case 0x82d938: return c->x<=0x1fff;
    case 0x82d94d: {
      uint16_t idx=r54_get(w,c,0x1c);
      if(idx>0x1e00 || c->x>0x1fff) return false;
      uint16_t y=wram_r16(w,(uint16_t)(0xd2u+idx));
      return y<=0x1ff8;
    }
    case 0x82d96e: return c->y<=0x1ff8 && c->x<=0x1fff;
    case 0x82d9b4: {
      uint16_t idx=r54_get(w,c,0x1c);
      return idx<=0x100 && rom_has(rom,((uint32_t)c->db<<16)|(0xdb42u+idx),2);
    }
    case 0x82d9c3: return c->s>=0x102;
    case 0x82d9d5: return c->s<0x1ffe;
    default: return true;
  }
}
void native_r54_d9_cluster(Wram* w,const Rom* rom,PortCpu* c,NativeHotWork* work) {
  (void)work;
  switch(c->pc) {
    case 0x82d8db: { // bump slot counter, test whether animation needs a dispatch
      lda(c,r54_get(w,c,0)); c->a=asl16(c,c->a); ldx(c,c->a);
      const uint16_t v=(uint16_t)(wram_r16(w,(uint16_t)(0x1f98u+c->x))+1);
      wram_w16(w,(uint16_t)(0x1f98u+c->x),v); set_nz16(c,v);
      lda(c,wram_r16(w,0x20));lda(c,(uint16_t)(c->a&1));
      cmp16(c,c->a,r54_get(w,c,0));
      if(flag(c,PORT_P_Z)) c->pc=0x82d8f3u;
      else {lda(c,1);c->pc=0x82d8efu;}break;
    }
    case 0x82d8fd: {
      ldx(c,r54_get(w,c,0x1c));lda(c,wram_r16(w,(uint16_t)(0x1f98u+c->x)));
      c->pc=flag(c,PORT_P_Z)?0x82d9b4u:0x82d907u;break;
    }
    case 0x82d907: {
      lda(c,wram_r16(w,0x1d52));cmp16(c,c->a,r54_get(w,c,0x1e));
      if(flag(c,PORT_P_Z)) c->pc=0x82d913u;
      else {r54_put(w,c,0x1e,c->a);c->pc=0x82d910u;}break;
    }
    case 0x82d913: {
      lda(c,wram_r16(w,0x6e30));r54_put(w,c,0x18,c->a);
      r54_dec_dp(w,c,0x18);c->pc=flag(c,PORT_P_N)?0x82d91du:0x82d92au;break;
    }
    case 0x82d91d: {
      ldy(c,r54_get(w,c,0x08));lda(c,0xffe0u);
      wram_w16(w,(uint16_t)(2+c->y),c->a);
      wram_w16(w,(uint16_t)(6+c->y),c->a);
      c->pc=0x82d8f6u;break;
    }
    case 0x82d92a: {
      lda(c,r54_get(w,c,0x0a));lda(c,(uint16_t)(c->a+1));
      cmp16(c,c->a,wram_r16(w,0x6e30));
      if(!flag(c,PORT_P_Z) && flag(c,PORT_P_C)) lda(c,1);
      c->pc=0x82d938u;break;
    }
    case 0x82d938: {
      r54_put(w,c,0x0a,c->a);ldx(c,c->a);ldx(c,(uint16_t)(c->x-1));
      lda(c,wram_r16(w,0x605a+c->x));lda(c,(uint16_t)(c->a&0x80u));
      c->pc=flag(c,PORT_P_Z)?0x82d945u:0x82d919u;break;
    }
    case 0x82d945: {
      lda(c,c->x);c->a=asl16(c,c->a);c->a=asl16(c,c->a);ldx(c,c->a);
      r54_put(w,c,0x14,0);r54_put(w,c,0x16,0);
      c->pc=0x82d94du;break;
    }
    case 0x82d94d: {
      ldy(c,r54_get(w,c,0x1c));
      lda(c,wram_r16(w,(uint16_t)(0xd2u+c->y)));ldy(c,c->a);
      set_c(c,true);lda(c,wram_r16(w,(uint16_t)(2+c->y)));
      c->a=sbc16(c,c->a,wram_r16(w,0x6df4+c->x));
      if(flag(c,PORT_P_N)) {
        lda(c,(uint16_t)(c->a^0xffffu));lda(c,(uint16_t)(c->a+1));r54_inc_dp(w,c,0x14);
      }
      cmp16(c,c->a,0x0180);
      if(flag(c,PORT_P_C)) {c->pc=0x82d919u;break;}
      r54_lsr(c);r54_lsr(c);r54_lsr(c);r54_lsr(c);
      r54_put(w,c,0x10,c->a);c->pc=0x82d96eu;break;
    }
    case 0x82d96e: {
      set_c(c,true);lda(c,wram_r16(w,(uint16_t)(6+c->y)));
      c->a=sbc16(c,c->a,wram_r16(w,0x6df6+c->x));
      if(flag(c,PORT_P_N)) {
        lda(c,(uint16_t)(c->a^0xffffu));lda(c,(uint16_t)(c->a+1));r54_inc_dp(w,c,0x16);
      }
      cmp16(c,c->a,0x0180);
      if(flag(c,PORT_P_C)) {c->pc=0x82d919u;break;}
      r54_lsr(c);r54_lsr(c);r54_lsr(c);r54_lsr(c);
      r54_put(w,c,0x12,c->a);c->pc=0x82d989u;break;
    }
    case 0x82d989: {
      lda(c,r54_get(w,c,0x14));const bool negative=!flag(c,PORT_P_Z);
      set_c(c,!negative);lda(c,r54_get(w,c,0x0c));
      c->a=negative?adc16(c,c->a,r54_get(w,c,0x10)):sbc16(c,c->a,r54_get(w,c,0x10));
      c->pc=0x82d999u;break;
    }
    case 0x82d999: {
      ldy(c,r54_get(w,c,0x08));wram_w16(w,(uint16_t)(2+c->y),c->a);
      lda(c,r54_get(w,c,0x16));const bool negative=!flag(c,PORT_P_Z);
      set_c(c,!negative);lda(c,r54_get(w,c,0x0e));
      c->a=negative?adc16(c,c->a,r54_get(w,c,0x12)):sbc16(c,c->a,r54_get(w,c,0x12));
      c->pc=0x82d9aeu;break;
    }
    case 0x82d9ae: {
      wram_w16(w,(uint16_t)(6+c->y),c->a);c->pc=0x82d8f6u;break;
    }
    case 0x82d9b4: {
      ldx(c,r54_get(w,c,0x1c));
      lda(c,bus_r16(w,rom,((uint32_t)c->db<<16)|(uint16_t)(0xdb42u+c->x)));
      lda(c,(uint16_t)(c->a^wram_r16(w,0x136e)));
      wram_w16(w,0x136e,c->a);c->pc=0x82d9bfu;break;
    }
    case 0x82d9c3: {
      lda(c,c->x);c->a=asl16(c,c->a);ldy(c,c->a);
      lda(c,0x82);push16(w,c,c->a);
      lda(c,0xdb38);ldx(c,r54_get(w,c,2));ldy(c,r54_get(w,c,4));
      c->pc=0x82d9d1u;break;
    }
    case 0x82d9d5: {
      lda(c,pull16(w,c));lda(c,r54_get(w,c,8));c->pc=0x82d9d8u;break;
    }
    case 0x82d9dc: {
      lda(c,r54_get(w,c,0));
      if(flag(c,PORT_P_Z)) {lda(c,0xdaa0);ldy(c,0x82);c->pc=0x82d9e6u;}
      else c->pc=0x82d9ebu;
      break;
    }
    case 0x82d9eb: {
      lda(c,0xdade);ldy(c,0x82);c->pc=0x82d9f1u;break;
    }
    default: break;
  }
}


// R55: direct native 65816-to-C slices at remaining actor/setup hotspots.
// These are bounded basic blocks, never external JSL/JSR/return execution.
// Non-WRAM direct/absolute modes and unknown register widths return to ROM.
static uint16_t r55_dp(const Wram* w,const PortCpu* c,uint16_t o) {
  return wram_r16(w,(uint16_t)(c->d+o));
}
static void r55_store(Wram* w,const PortCpu* c,uint16_t o,uint16_t v) {
  wram_w16(w,(uint16_t)(c->d+o),v);
}
static void r55_stz(Wram* w,const PortCpu* c,uint16_t o) {
  r55_store(w,c,o,0);
}
static void r55_or(PortCpu* c,uint16_t value) {lda(c,(uint16_t)(c->a|value));}
static void r55_and(PortCpu* c,uint16_t value) {lda(c,(uint16_t)(c->a&value));}
static void r55_inc_dp(Wram*w,PortCpu*c,uint16_t off) {
  uint16_t v=(uint16_t)(r55_dp(w,c,off)+1);
  r55_store(w,c,off,v);set_nz16(c,v);
}
static void r55_dec_dp(Wram*w,PortCpu*c,uint16_t off) {
  uint16_t v=(uint16_t)(r55_dp(w,c,off)-1);
  r55_store(w,c,off,v);set_nz16(c,v);
}
static bool r55_entry(uint32_t pc) {
  switch(pc) {
    case 0x839843: case 0x83984f: case 0x83985d: case 0x839865:
    case 0x839880: case 0x83989b: case 0x8398aa: case 0x8398d2:
    case 0x8398f1: case 0x82988d: case 0x829890: case 0x8298b0:
    case 0x8298ce: case 0x82991a: case 0x829927: case 0x82993b:
    case 0x82993f: return true;
    default: return false;
  }
}
bool native_r55_supported(const Wram*w,const Rom*rom,const PortCpu*c) {
  if(!r55_entry(c->pc) || !rom_has(rom,c->pc,2) ||
     c->d>0x1e00 || (c->p&(PORT_P_M|PORT_P_X|PORT_P_D)) ||
     !(c->db<0x40 || (c->db>=0x80 && c->db<0xc0)) ||
     c->s<0x100 || c->s>=0x2000) return false;
  switch(c->pc) {
    case 0x83984f:
      return (uint32_t)r55_dp(w,c,6)+0x605au<0x1fffeu;
    case 0x839880: case 0x8398aa: case 0x8398d2: case 0x8398f1:
    case 0x829890: case 0x8298b0:
      if ((c->pc==0x8398d2 || c->pc==0x8398f1 || c->pc==0x8398aa)
          ? r55_dp(w,c,8)>0x1ff6u : c->y>0x1feeu) return false;
      if (c->pc==0x8398aa) {
        const uint16_t index=(uint16_t)(r55_dp(w,c,0x10)+1);
        if(index>0x0100) return false;
        // The actor animation table is ROM, not mirrored low WRAM.
        if(!rom_has(rom,((uint32_t)c->db<<16)|(uint16_t)(0x992bu+(index<<1)),2))return false;
      }
      return true;
    default: return true;
  }
}
void native_r55_actor_fragments(Wram*w,const Rom*rom,PortCpu*c,NativeHotWork*work) {
  (void)work;
  switch(c->pc) {
    case 0x839843: { // Actor animation timer: LDA $1FF6; BEQ
      lda(c,wram_r16(w,0x1ff6));
      c->pc=flag(c,PORT_P_Z)?0x839865:0x839848;break;
    }
    case 0x83984f: { // Live 7E actor state byte: LDX dp; LDA long,X; compare
      ldx(c,r55_dp(w,c,6));
      lda(c,wram_r16(w,(uint32_t)0x605a+c->x));
      r55_and(c,0xff);cmp16(c,c->a,0x80);
      c->pc=flag(c,PORT_P_Z)?0x839864:0x83985d;break;
    }
    case 0x83985d: { // Re-check actor count and return to the event hook
      lda(c,wram_r16(w,0x1ff6));
      c->pc=flag(c,PORT_P_Z)?0x839865:0x839848;break;
    }
    case 0x839865: { // Next animation step; stop at engine's JSR
      uint16_t v=(uint16_t)(wram_r16(w,0x1ff6)+1);
      wram_w16(w,0x1ff6,v);set_nz16(c,v);
      set_c(c,false);lda(c,wram_r16(w,0xde));c->a=adc16(c,c->a,0x000a);
      wram_w16(w,0xde,c->a);
      lda(c,6);r55_store(w,c,0x1c,c->a);
      lda(c,0xdccf);ldx(c,0x8f);c->pc=0x83987d;break;
    }
    case 0x839880: { // Actor state setup, before $80:8353 engine call
      ldy(c,r55_dp(w,c,8));
      lda(c,wram_r16(w,c->y));r55_or(c,8);
      wram_w16(w,c->y,c->a);
      r55_stz(w,c,0x1e);
      lda(c,4);r55_store(w,c,0x28,c->a);r55_stz(w,c,0x2a);
      lda(c,1);c->pc=0x839897;break;
    }
    case 0x83989b: { // Transition condition before actor-loop update
      lda(c,r55_dp(w,c,0x1e));
      if(!flag(c,PORT_P_N)) {c->pc=0x8398aa;break;}
      lda(c,wram_r16(w,0x1fb8));r55_or(c,wram_r16(w,0x1fba));
      c->pc=flag(c,PORT_P_Z)?0x8398aa:0x839916;break;
    }
    case 0x8398aa: { // Mutating actor animation loop: no engine calls
      ldy(c,r55_dp(w,c,8));set_c(c,false);
      lda(c,wram_r16(w,(uint16_t)(c->y+4)));
      c->a=adc16(c,c->a,r55_dp(w,c,0x28));
      wram_w16(w,(uint16_t)(c->y+4),c->a);
      if(flag(c,PORT_P_Z)) {c->pc=0x8398d2;break;}
      r55_dec_dp(w,c,0x2a);lda(c,r55_dp(w,c,0x2a));r55_and(c,7);
      if(!flag(c,PORT_P_Z)) {c->pc=0x839894;break;}
      r55_dec_dp(w,c,0x28);r55_inc_dp(w,c,0x10);
      lda(c,r55_dp(w,c,0x10));c->a=asl16(c,c->a);ldx(c,c->a);
      lda(c,bus_r16(w,rom,((uint32_t)c->db<<16)|(uint16_t)(0x992b+c->x)));
      ldy(c,r55_dp(w,c,8));wram_w16(w,(uint16_t)(c->y+8),c->a);
      c->pc=0x839894;break;
    }
    case 0x8398d2: { // Reset display state before JSL
      lda(c,4);r55_store(w,c,0x28,c->a);
      r55_stz(w,c,0x2a);r55_stz(w,c,0x10);
      ldy(c,r55_dp(w,c,8));lda(c,0xdccf);
      wram_w16(w,(uint16_t)(c->y+8),c->a);
      lda(c,4);c->pc=0x8398e6;break;
    }
    case 0x8398f1: { // Finish/transition branches after audio JSL
      ldy(c,r55_dp(w,c,8));lda(c,0xdcf0);
      wram_w16(w,(uint16_t)(c->y+8),c->a);
      lda(c,r55_dp(w,c,0x1e));
      if(flag(c,PORT_P_Z)) {c->pc=0x839894;break;}
      if(flag(c,PORT_P_N)) {c->pc=0x839916;break;}
      cmp16(c,c->a,1);
      if(flag(c,PORT_P_Z)) {c->pc=0x839908;break;}
      r55_stz(w,c,0x1e);c->pc=0x839894;break;
    }
    case 0x82988d: { // Return value becomes actor slot, then indexed Y
      r55_store(w,c,8,c->a);ldy(c,c->a);c->pc=0x829890;break;
    }
    case 0x829890: { // Full actor data setup, stop before next global load
      lda(c,r55_dp(w,c,0));r55_store(w,c,0x0e,c->a);
      wram_w16(w,(uint16_t)(c->y+2),c->a);
      lda(c,0);wram_w16(w,(uint16_t)(c->y+4),c->a);
      lda(c,r55_dp(w,c,2));r55_store(w,c,0x10,c->a);
      wram_w16(w,(uint16_t)(c->y+6),c->a);
      lda(c,0xeb59);wram_w16(w,(uint16_t)(c->y+8),c->a);
      lda(c,0x8f);wram_w16(w,(uint16_t)(c->y+10),c->a);
      c->pc=0x8298b0;break;
    }
    case 0x8298b0: { // Build initial actor record, keep $80:8475 call in ROM
      lda(c,wram_r16(w,8));wram_w16(w,(uint16_t)(c->y+12),c->a);
      lda(c,3);wram_w16(w,(uint16_t)(c->y+14),c->a);
      lda(c,0x8000);r55_or(c,wram_r16(w,c->y));
      wram_w16(w,c->y,c->a);
      lda(c,0x0c00);wram_w16(w,(uint16_t)(c->y+16),c->a);
      lda(c,1);c->pc=0x8298ce;break;
    }
    case 0x8298ce: { // Actor thread-field reset before JSL
      r55_store(w,c,0x0c,c->a);
      r55_stz(w,c,0x1e);r55_stz(w,c,0x1c);
      r55_stz(w,c,0x0a);r55_stz(w,c,0x20);r55_stz(w,c,0x7e);
      lda(c,0x9a6d);ldy(c,0x82);c->pc=0x8298e0;break;
    }
    case 0x82991a: { // Event-clock advance before original JSR
      set_c(c,false);lda(c,wram_r16(w,0xde));
      c->a=adc16(c,c->a,0x0012);wram_w16(w,0xde,c->a);
      c->pc=0x829924;break;
    }
    case 0x829927: { // Actor transition before JSL
      r55_stz(w,c,0x20);lda(c,2);c->pc=0x82992c;break;
    }
    case 0x82993b: { // Animation response branch
      lda(c,r55_dp(w,c,0x0a));c->pc=flag(c,PORT_P_Z)?0x829927:0x82993f;break;
    }
    case 0x82993f: { // Staged transition dispatcher
      ldx(c,0x200);lda(c,r55_dp(w,c,0x20));
      c->pc=flag(c,PORT_P_Z)?0x829969:0x829946;break;
    }
    default: break;
  }
}

// R59: Instruction-exact 65816 fragments from the collision dispatcher,
// thread handler, actor distance check and trap callback. Original JSL/RTL
// instructions are left to the existing machine owner at every boundary.
static uint16_t r59_dp_r(const Wram *w,const PortCpu*c,unsigned o){return wram_r16(w,(uint16_t)(c->d+o));}
static void r59_dp_w(Wram*w,const PortCpu*c,unsigned o,uint16_t v){wram_w16(w,(uint16_t)(c->d+o),v);}
static void r59_nz(PortCpu*c,uint16_t v){set_nz16(c,v);}
static bool r59_entry(uint32_t pc){switch(pc){
case 0x80be9e:case 0x80bea0:case 0x80bea8:case 0x80beb8:
case 0x808483:case 0x808486:
case 0x81f25d:case 0x81f276:case 0x81f27a:
case 0x829845:case 0x82984d:case 0x82984f:case 0x829852:case 0x829854:
return true;default:return false;}}
bool native_r59_supported(const Wram*w,const Rom*rom,const PortCpu*c){
  if(!r59_entry(c->pc)||!rom_has(rom,c->pc,2)||
     (c->p&(PORT_P_M|PORT_P_X|PORT_P_D))||c->s<0x100||c->s>=0x2000)return false;
  switch(c->pc){
    case 0x808483:case 0x808486:
      return (c->db<0x40||(c->db>=0x80&&c->db<0xc0))&&c->x<=0x0cceu;
    case 0x80bea0:
    case 0x80be9e:
      if(c->d>0x1e00) return false;
      return c->x<=0x1fd0u-c->d;
    case 0x80bea8:case 0x80beb8:case 0x81f25d:
    case 0x81f276:case 0x81f27a:
    case 0x829845:case 0x82984d:case 0x82984f:
    case 0x829852:case 0x829854:
      return c->d<=0x1e00;
    default:return false;
  }
}
void native_r59_collision_fragments(Wram*w,const Rom*rom,PortCpu*c,NativeHotWork*work){
  (void)rom;(void)work;
  switch(c->pc){
  case 0x80be9e:
    r59_dp_w(w,c,0x3e,c->x); // STX $3E
    // fall through, now in the second actor's $0E/$0C fields
  case 0x80bea0:
    lda(c,wram_r16(w,(uint16_t)(c->d+0x0e + c->x)));
    r59_dp_w(w,c,0x42,c->a);
    lda(c,wram_r16(w,(uint16_t)(c->d+0x0c + c->x)));
    r59_dp_w(w,c,0x44,c->a);
    // fall through
  case 0x80bea8:
    lda(c,r59_dp_r(w,c,0x40));r59_dp_w(w,c,0x78,c->a);
    lda(c,r59_dp_r(w,c,0x3e));r59_dp_w(w,c,0x76,c->a);
    ldx(c,r59_dp_r(w,c,0x48));ldy(c,r59_dp_r(w,c,0x42));
    c->pc=0x80beb4;break; // original JSL $80:8480
  case 0x80beb8:
    lda(c,r59_dp_r(w,c,0x3e));r59_dp_w(w,c,0x78,c->a);
    lda(c,r59_dp_r(w,c,0x40));r59_dp_w(w,c,0x76,c->a);
    ldx(c,r59_dp_r(w,c,0x44));ldy(c,r59_dp_r(w,c,0x46));
    c->pc=0x80bec4;break; // second original JSL $80:8480
  case 0x808483:
    // ORA source comes from prior LDA $1300,X; entry at $8483 starts
    // with whatever A the 65816 currently holds.
    c->a=(uint16_t)(c->a|wram_r16(w,(uint16_t)(0x1330+c->x)));
    r59_nz(c,c->a);
    c->pc=flag(c,PORT_P_Z)?0x8084b0:0x808488;break;
  case 0x808486:
    c->pc=flag(c,PORT_P_Z)?0x8084b0:0x808488;break;
  case 0x81f25d: {
    cmp16(c,c->a,3); if(flag(c,PORT_P_Z)) goto f276;
    cmp16(c,c->a,4); if(flag(c,PORT_P_Z)) goto f276;
    cmp16(c,c->a,0x61);if(flag(c,PORT_P_Z)) goto f27a;
    lda(c,(uint16_t)(c->a&0x7fff));cmp16(c,c->a,0x5c);
    if(flag(c,PORT_P_C)) goto f276;
    set_c(c,false);c->pc=0x81f275;break;
  }
  case 0x81f276: goto f276;
  case 0x81f27a: goto f27a;
f276:
    {uint16_t v=(uint16_t)(r59_dp_r(w,c,0x40)-1u);
     r59_dp_w(w,c,0x40,v);r59_nz(c,v);
     if(!flag(c,PORT_P_N)) {set_c(c,true);c->pc=0x81f27d;break;}}
f27a:
    {uint16_t v=(uint16_t)(r59_dp_r(w,c,0x1c)-1u);
     r59_dp_w(w,c,0x1c,v);r59_nz(c,v);
     set_c(c,true);c->pc=0x81f27d;break;}
  case 0x829845:
    ldx(c,r59_dp_r(w,c,0x0e));ldy(c,r59_dp_r(w,c,0x10));
    c->pc=0x829849;break; // original JSL $80:B123
  case 0x82984d:
    r59_dp_w(w,c,0x22,c->x);
    // fall through
  case 0x82984f:
    cmp16(c,c->a,0x70);
    // fall through
  case 0x829852:
    c->pc=flag(c,PORT_P_C)?0x8297e0:0x829857;break;
  case 0x829854:
    c->pc=0x8297e0;break;
  default:break;
  }
}


// R60: longer connected fallback blocks through real collision setup,
// thread/actor response staging and actor bookkeeping. Never absorb a JSL.
// No existing native entry is replaced; the whole-routine guards keep priority.
static uint16_t r60_dp(const Wram*w,const PortCpu*c,unsigned v){return wram_r16(w,(uint16_t)(c->d+v));}
static void r60_store(Wram*w,const PortCpu*c,unsigned v,uint16_t x){wram_w16(w,(uint16_t)(c->d+v),x);}
static bool r60_entry(uint32_t pc){switch(pc){
 case 0x80be91:case 0x80be93:case 0x80be95:case 0x80be97:
 case 0x80be99:case 0x80be9b:
 case 0x81f1cd:case 0x81f1d0:
 case 0x82993d:case 0x829942:case 0x829944:
 case 0x82994a:case 0x82994d:case 0x82994f:case 0x829952:
 case 0x829955:case 0x829958:case 0x82995b:
 return true; default:return false;}}
bool native_r60_supported(const Wram*w,const Rom*rom,const PortCpu*c){
 if(!r60_entry(c->pc)||!rom_has(rom,c->pc,2)||
   (c->p&(PORT_P_M|PORT_P_X|PORT_P_D))||
   c->s<0x100||c->s>=0x2000||c->d>0x1e00||
   !(c->db<0x40||(c->db>=0x80&&c->db<0xc0)))return false;
 // DB must map abs-indexed data to ROM above $8000 and WRAM below $2000.
 if(c->pc==0x81f1cd||c->pc==0x81f1d0) {
   if(c->y>0x1ff6)return false;
   if(c->pc==0x81f1cd&&c->x>0x0d54)return false;
 }
 if(c->pc>=0x80be91&&c->pc<=0x80be9b){
   if(c->pc<=0x80be97 && (uint32_t)c->d+c->x+0x0f>=0x2000)return false;
   if(c->pc<=0x80be9b &&
       r60_dp(w,c,0x3c)>0xc7e)return false;
   // Starting at BE99+ reads $1380,Y after LDY $3C; BE9B uses live Y.
   if(c->pc==0x80be9b&&c->y>0xc7e)return false;
 }
 if(c->pc>=0x82994a&&c->pc<=0x82995b) {
   if(c->pc==0x82994a&&c->db>=0x40&&c->db<0x80)return false;
   if(c->pc<=0x82994d){
     if(r60_dp(w,c,0x08)>0x1ff0)return false;
   } else if(c->y>0x1ff0)return false;
 }
 return true;
}
void native_r60_connected_blocks(Wram*w,const Rom*rom,PortCpu*c,NativeHotWork*work){
 (void)work;
 switch(c->pc){
 case 0x80be91:
   lda(c,wram_r16(w,(uint16_t)(c->d+0x0e + c->x)));
   // fall through
 case 0x80be93:
   r60_store(w,c,0x46,c->a);
   // fall through
 case 0x80be95:
   lda(c,wram_r16(w,(uint16_t)(c->d+0x0c+c->x)));
   // fall through
 case 0x80be97:
   r60_store(w,c,0x48,c->a);
   // fall through
 case 0x80be99:
   ldy(c,r60_dp(w,c,0x3c));
   // fall through
 case 0x80be9b:
   ldx(c,wram_r16(w,(uint16_t)(0x1380+c->y)));
   c->pc=0x80be9e;break; // R59 owns the next connected portion.
 case 0x81f1cd:
   lda(c,bus_r16(w,rom,((uint32_t)c->db<<16)|(uint16_t)(0xf2aa+c->x)));
   // fall through
 case 0x81f1d0:
   wram_w16(w,(uint16_t)(0x0008+c->y),c->a);
   c->pc=0x81f1d3;break;
 case 0x82993d:
   if(flag(c,PORT_P_Z)){c->pc=0x829927;break;}
   // fall through
 case 0x829942: // Direct entry is after the existing R55 LDX #$200.
   if(c->pc==0x82993d)ldx(c,0x0200);
   lda(c,r60_dp(w,c,0x20));
   // fall through
 case 0x829944:
   c->pc=flag(c,PORT_P_Z)?0x829969:0x829946;break;
 case 0x82994a: {
   uint16_t v=(uint16_t)(wram_r16(w,0x1f86)+1);
   wram_w16(w,0x1f86,v);set_nz16(c,v);
   // fall through
 }
 case 0x82994d:
   ldy(c,r60_dp(w,c,0x08));
   // fall through
 case 0x82994f:
   lda(c,0);
   // fall through
 case 0x829952:
   wram_w16(w,(uint16_t)(0x0e + c->y),c->a);
   // fall through
 case 0x829955:
   lda(c,0x008f);
   // fall through
 case 0x829958:
   wram_w16(w,(uint16_t)(0x0a+c->y),c->a);
   // fall through
 case 0x82995b:
   lda(c,0x0021);
   c->pc=0x82995e;break; // JSL $80:CC3B stays on original owner.
 default:break;
 }
}

// R61: bounded native stack/callback epilogues and actor-state branches.
// Calls, returns, 8-bit width transitions and unsupported state stay in ROM.
static uint16_t r61_dp(const Wram*w,const PortCpu*c,unsigned off){return wram_r16(w,(uint16_t)(c->d+off));}
static bool r61_entry(uint32_t pc){switch(pc){
 case 0x808488:case 0x808489:case 0x80848a:
 case 0x8084a3:case 0x8084a5:case 0x8084a6:case 0x8084a8:
 case 0x8084ab:case 0x8084ae:case 0x8084af:
 case 0x81f1d3:case 0x81f1d5:case 0x81f1d7:case 0x81f1d9:
 case 0x81f1db:case 0x81f1de:case 0x81f1e1:
 case 0x829887:case 0x829930:case 0x829933:case 0x829935:
 case 0x829936:
 return true; default:return false;}}
bool native_r61_supported(const Wram*w,const Rom*rom,const PortCpu*c){
 (void)w;
 if(!r61_entry(c->pc)||!rom_has(rom,c->pc,1)||
    (c->p&(PORT_P_M|PORT_P_X|PORT_P_D))||
    c->s<0x110||c->s>=0x1ff0||c->d>0x1e00)return false;
 // Indexed absolute operands are in WRAM only for mirror banks.
 if(c->pc==0x8084a8||c->pc==0x8084ab){
    if(!(c->db<0x40||(c->db>=0x80&&c->db<0xc0))||c->x>0x0e7e)return false;
 }
 if(c->pc==0x81f1d3||c->pc==0x81f1d5||c->pc==0x81f1d7||
    c->pc==0x81f1d9||c->pc==0x81f1db){
    if(!(c->db<0x40||(c->db>=0x80&&c->db<0xc0)))return false;
 }
 return true;
}
void native_r61_thread_actor(Wram*w,const Rom*rom,PortCpu*c,NativeHotWork*work){
 (void)rom;(void)work;
 switch(c->pc){
 case 0x808488: push8(w,c,c->db); // PHB
   // fall through
 case 0x808489: push16(w,c,c->d); // PHD
   // fall through
 case 0x80848a: push16(w,c,c->x);c->pc=0x80848b;break; // SEP M in ROM
 case 0x8084a3: lda(c,c->y);c->pc=0x8084a4;break; // TYA, original RTL
 case 0x8084a5: ldx(c,pull16(w,c)); // PLX
   // fall through
 case 0x8084a6:
   c->pc=flag(c,PORT_P_C)?0x8084a8:0x8084ae;break; // BCC
 case 0x8084a8: lda(c,0x8000); // LDA #$8000
   // fall through
 case 0x8084ab: wram_w16(w,(uint16_t)(0x1180+c->x),c->a);
   c->pc=0x8084ae;break;
 case 0x8084ae: c->d=pull16(w,c);set_nz16(c,c->d); // PLD
   // fall through
 case 0x8084af: c->db=pull8(w,c);set_nz8(c,c->db);
   c->pc=0x8084b0;break; // RTL still ROM-owned
 case 0x81f1d3: lda(c,r61_dp(w,c,0x1c)); // LDA dp $1C
   // fall through
 case 0x81f1d5:
   if(flag(c,PORT_P_N))goto f1db; // BMI +4
   // fall through
 case 0x81f1d7:{
   uint16_t v=(uint16_t)(r61_dp(w,c,0x3e)-1u);
   wram_w16(w,(uint16_t)(c->d+0x3e),v);set_nz16(c,v); // DEC dp $3E
   }
   // fall through
 case 0x81f1d9:
   if(!flag(c,PORT_P_Z)){c->pc=0x81f166;break;} // BNE f166
   // fall through
 case 0x81f1db:
 f1db:{uint16_t v=(uint16_t)(wram_r16(w,0x1f90)-1u);
   wram_w16(w,0x1f90,v);set_nz16(c,v);}
   // fall through
 case 0x81f1de: lda(c,0); // LDA #$0000
   // fall through
 case 0x81f1e1: ldy(c,c->a);c->pc=0x81f1e2;break; // original JSL
 case 0x829887:
   c->pc=flag(c,PORT_P_C)?0x8298e5:0x829889;break;
 case 0x829930: push16(w,c,0x9937); // PEA $9937
   // fall through
 case 0x829933: lda(c,r61_dp(w,c,0x1a)); // LDA dp $1A
   // fall through
 case 0x829935: c->a=(uint16_t)(c->a-1u);set_nz16(c,c->a); // DEC A
   // fall through
 case 0x829936: push16(w,c,c->a);c->pc=0x829937;break; // original RTS
 default:break;
 }
}

// R62 native actor direction, state and movement-control chain.
// All paths are instruction-aligned and terminate before external JSL calls.
static uint16_t r62_dp(const Wram*w,const PortCpu*c,unsigned off){return wram_r16(w,(uint16_t)(c->d+off));}
static void r62_stdp(Wram*w,PortCpu*c,unsigned off,uint16_t v){wram_w16(w,(uint16_t)(c->d+off),v);set_nz16(c,v);}
static bool r62_entry(uint32_t pc){switch(pc){
 case 0x81f175u: case 0x81f178u: case 0x81f17au: case 0x81f17du: case 0x81f17fu:
 case 0x81f182u: case 0x81f184u: case 0x81f186u: case 0x81f188u: case 0x81f18au:
 case 0x81f18du: case 0x81f18fu: case 0x81f191u: case 0x81f193u: case 0x81f195u:
 case 0x81f198u: case 0x81f19au: case 0x81f19cu: case 0x81f19eu: case 0x81f1a0u:
 case 0x81f1a3u: case 0x81f1a5u: case 0x81f1a7u: case 0x81f1a9u: case 0x81f1abu:
 case 0x81f1adu: case 0x81f1b0u: case 0x81f1b2u: case 0x81f1b5u: case 0x81f1b7u:
 case 0x81f1b8u: case 0x81f1bbu: case 0x81f1bdu: return true;default:return false;}}
bool native_r62_supported(const Wram*w,const Rom*rom,const PortCpu*c){
 if(!r62_entry(c->pc)||!rom_has(rom,c->pc,1)||
   (c->p&(PORT_P_M|PORT_P_X|PORT_P_D))||c->d>0x1e00)return false;
 // DP and indexed absolute/Y are WRAM-only in bank mirrors.
 if(!((c->db<0x40)||(c->db>=0x80&&c->db<0xc0)))return false;
 // Writes in second block must not touch hardware/ROM bank pages.
 if(c->pc>=0x81f1a9 && c->pc<=0x81f1b2){
   const uint16_t yy=(c->pc==0x81f1a9)?r62_dp(w,c,0x0a):c->y;
   if(yy>0x1ff6)return false;
 }
 return true;
}
void native_r62_actor_control(Wram*w,const Rom*rom,PortCpu*c,NativeHotWork*wk){
 (void)rom;(void)wk;
 switch(c->pc){
 case 0x81f175: cmp16(c,c->a,0x0400); // CMP #$0400
   // fall through
 case 0x81f178: if(flag(c,PORT_P_Z)){c->pc=0x81f1db;break;} // BEQ $F1DB
   // fall through
 case 0x81f17a: // BIT #$0008 affects Z, not N/V
   c->p=(uint8_t)((c->p&~PORT_P_Z)|((c->a&0x0008)?0:PORT_P_Z));
   // fall through
 case 0x81f17d: if(flag(c,PORT_P_Z)){c->pc=0x81f1b5;break;}
   // fall through
 case 0x81f17f: cmp16(c,c->a,0x0108);
   // fall through
 case 0x81f182: if(!flag(c,PORT_P_Z))goto f18a;
   // fall through
 case 0x81f184: r62_stdp(w,c,2,(uint16_t)(r62_dp(w,c,2)-1));
   // fall through
 case 0x81f186: r62_stdp(w,c,2,(uint16_t)(r62_dp(w,c,2)-1));
   // fall through
 case 0x81f188: c->pc=0x81f1a9;break;
 case 0x81f18a: f18a: cmp16(c,c->a,0x0408);
   // fall through
 case 0x81f18d: if(!flag(c,PORT_P_Z))goto f195;
   // fall through
 case 0x81f18f: r62_stdp(w,c,2,(uint16_t)(r62_dp(w,c,2)+1));
   // fall through
 case 0x81f191: r62_stdp(w,c,2,(uint16_t)(r62_dp(w,c,2)+1));
   // fall through
 case 0x81f193: c->pc=0x81f1a9;break;
 case 0x81f195: f195: cmp16(c,c->a,0x0208);
   // fall through
 case 0x81f198: if(!flag(c,PORT_P_Z))goto f1a0;
   // fall through
 case 0x81f19a: r62_stdp(w,c,0,(uint16_t)(r62_dp(w,c,0)-1));
   // fall through
 case 0x81f19c: r62_stdp(w,c,0,(uint16_t)(r62_dp(w,c,0)-1));
   // fall through
 case 0x81f19e: c->pc=0x81f1a9;break;
 case 0x81f1a0: f1a0: cmp16(c,c->a,0x0028);
   // fall through
 case 0x81f1a3: if(!flag(c,PORT_P_Z)){c->pc=0x81f1a9;break;}
   // fall through
 case 0x81f1a5: r62_stdp(w,c,0,(uint16_t)(r62_dp(w,c,0)+1));
   // fall through
 case 0x81f1a7: r62_stdp(w,c,0,(uint16_t)(r62_dp(w,c,0)+1));
   c->pc=0x81f1a9;break;
 case 0x81f1a9: ldy(c,r62_dp(w,c,0x0a));
   // fall through
 case 0x81f1ab: lda(c,r62_dp(w,c,0));
   // fall through
 case 0x81f1ad: wram_w16(w,(uint16_t)(2+c->y),c->a);
   // fall through
 case 0x81f1b0: lda(c,r62_dp(w,c,2));
   // fall through
 case 0x81f1b2: wram_w16(w,(uint16_t)(6+c->y),c->a);
   c->pc=0x81f1b5;break;
 case 0x81f1b5: lda(c,r62_dp(w,c,0x3c));
   // fall through
 case 0x81f1b7: c->a=(uint16_t)(c->a+1);set_nz16(c,c->a);
   // fall through
 case 0x81f1b8: cmp16(c,c->a,4);
   // fall through
 case 0x81f1bb: if(!flag(c,PORT_P_C)){c->pc=0x81f1c7;break;}
   // fall through
 case 0x81f1bd: lda(c,2);c->pc=0x81f1c0;break;
 default:break;
 }
}


// R63: actual post-callback actor and thread timing continuations.
// Never bypass JSL/JML or the original busy-wait branch.
static bool r63_entry(uint32_t pc) {switch(pc){
 case 0x81f1c4u:
 case 0x81f1c7u:
 case 0x81f1c9u:
 case 0x81f1cau:
 case 0x81f1cbu:
 case 0x81f1e6u:
 case 0x81f1edu:
 case 0x81f1eeu:
 case 0x81f1f1u:
 case 0x81f1f4u:
 case 0x81f1f9u:
 case 0x829962u:
 case 0x829969u:
 case 0x82996au:
 case 0x82996du:
 case 0x829970u:
 case 0x829975u: return true;default:return false;}}
bool native_r63_supported(const Wram*w,const Rom*rom,const PortCpu*c) {
 (void)w;
 if(!r63_entry(c->pc)||!rom_has(rom,c->pc,1)||
    (c->p&(PORT_P_M|PORT_P_X|PORT_P_D))||c->d>0x1e00)return false;
 // All accesses with $00DE or direct page occur in WRAM mirrors.
 if(!(c->db<0x40||(c->db>=0x80&&c->db<0xc0)))return false;
 return true;
}
static uint16_t r63_dp(const Wram*w,const PortCpu*c,unsigned n) {
 return wram_r16(w,(uint16_t)(c->d+n));
}
static void r63_store_dp(Wram*w,const PortCpu*c,unsigned n,uint16_t v) {
 wram_w16(w,(uint16_t)(c->d+n),v);
}
static void r63_lda(PortCpu*c,uint16_t v) {c->a=v;set_nz16(c,v);}
void native_r63_postcall(Wram*w,const Rom*rom,PortCpu*c,NativeHotWork*wk) {
 (void)rom;(void)wk;
 switch(c->pc) {
 case 0x81f1c4: r63_lda(c,0); // LDA #0, $80:CC3B returned
   // fall through
 case 0x81f1c7: r63_store_dp(w,c,0x3c,c->a); // STA $3C
   // fall through
 case 0x81f1c9: c->a=asl16(c,c->a); // ASL A
   // fall through
 case 0x81f1ca: c->x=c->a;set_nz16(c,c->x); // TAX
   // fall through
 case 0x81f1cb: c->y=r63_dp(w,c,0x0a);set_nz16(c,c->y); // LDY $0A
   c->pc=0x81f1cd;break; // R60 native continues
 case 0x81f1e6: r63_lda(c,0xf296);c->pc=0x81f1e9;break; // original JSL
 case 0x81f1ed: set_c(c,true); // SEC
   // fall through
 case 0x81f1ee: r63_lda(c,wram_r16(w,0xde)); // LDA $00DE
   // fall through
 case 0x81f1f1: c->a=sbc16(c,c->a,8); // SBC #8
   // fall through
 case 0x81f1f4: wram_w16(w,0xde,c->a); // STA $00DE
   c->pc=0x81f1f7;break; // original BMI wait
 case 0x81f1f9: r63_lda(c,r63_dp(w,c,0x0a));
   c->pc=0x81f1fb;break; // original JML $80:BE41
 case 0x829962: r63_lda(c,0x997c);c->pc=0x829965;break; // original JSL
 case 0x829969: set_c(c,true); // SEC
   // fall through
 case 0x82996a: r63_lda(c,wram_r16(w,0xde)); // LDA $00DE
   // fall through
 case 0x82996d: c->a=sbc16(c,c->a,0x12); // SBC #$12
   // fall through
 case 0x829970: wram_w16(w,0xde,c->a); // STA $00DE
   c->pc=0x829973;break; // original BMI wait
 case 0x829975: r63_lda(c,r63_dp(w,c,0x08));
   c->pc=0x829977;break; // original JML $80:BE41
 default:break;
 }
}


// R64: $81:F204-$81:F249, actor record initialization after ROM JSL $80:BE0C.
// Stop before original LDA/JSL; never emulate callbacks or machine timing.
static bool r64_actor_entry(uint32_t pc) {
  switch (pc) {
    case 0x81f204u:
    case 0x81f206u:
    case 0x81f208u:
    case 0x81f20bu:
    case 0x81f20cu:
    case 0x81f20eu:
    case 0x81f211u:
    case 0x81f214u:
    case 0x81f217u:
    case 0x81f219u:
    case 0x81f21cu:
    case 0x81f21fu:
    case 0x81f222u:
    case 0x81f225u:
    case 0x81f228u:
    case 0x81f22bu:
    case 0x81f22eu:
    case 0x81f231u:
    case 0x81f234u:
    case 0x81f237u:
    case 0x81f23au:
    case 0x81f23du:
    case 0x81f23fu:
    case 0x81f241u:
    case 0x81f244u:
    case 0x81f246u:
    case 0x81f249u:
    return true;
    default: return false;
  }
}
bool native_r64_supported(const Wram *w,const Rom *rom,const PortCpu *c) {
  (void)w;
  if (!r64_actor_entry(c->pc) || !rom_has(rom,0x81f204u,0x47u) ||
      (c->p & (PORT_P_M|PORT_P_X|PORT_P_D)) || c->d>0x1e00u ||
      c->y>0x1feeu ||
      !(c->db<0x40u || (c->db>=0x80u && c->db<0xc0u))) return false;
  return true;
}
static uint16_t r64_dp(const Wram *w,const PortCpu *c,unsigned off) {
  return wram_r16(w,(uint16_t)(c->d+off));
}
static void r64_put_dp(Wram *w,const PortCpu *c,unsigned off,uint16_t v) {
  wram_w16(w,(uint16_t)(c->d+off),v);
}
static void r64_lda(PortCpu *c,uint16_t v) {c->a=v;set_nz16(c,v);}
static void r64_sta_y(Wram *w,const PortCpu *c,unsigned off) {
  wram_w16(w,(uint16_t)(c->y+off),c->a);
}
void native_r64_actor_record(Wram *w,const Rom *rom,PortCpu *c,NativeHotWork *work) {
  (void)rom;(void)work;
  switch(c->pc) {
    case 0x81f204u: r64_put_dp(w,c,0x0a,c->a);  // STA $0A
      // fall through
    case 0x81f206u: r64_put_dp(w,c,0x08,c->a);  // STA $08
      // fall through
    case 0x81f208u: { // INC $1F90 (absolute, DB mapped to low WRAM)
      uint16_t v=(uint16_t)(wram_r16(w,0x1f90u)+1u);
      wram_w16(w,0x1f90u,v);set_nz16(c,v);
    }
      // fall through
    case 0x81f20bu: set_c(c,false); // CLC
      // fall through
    case 0x81f20cu: r64_lda(c,r64_dp(w,c,0)); // LDA $00
      // fall through
    case 0x81f20eu: r64_sta_y(w,c,2); // STA $0002,Y
      // fall through
    case 0x81f211u: r64_lda(c,0); // LDA #$0000
      // fall through
    case 0x81f214u: r64_sta_y(w,c,4); // STA $0004,Y
      // fall through
    case 0x81f217u: r64_lda(c,r64_dp(w,c,2)); // LDA $02
      // fall through
    case 0x81f219u: r64_sta_y(w,c,6); // STA $0006,Y
      // fall through
    case 0x81f21cu: r64_lda(c,0xd659); // LDA #$D659
      // fall through
    case 0x81f21fu: r64_sta_y(w,c,8); // STA $0008,Y
      // fall through
    case 0x81f222u: r64_lda(c,0x008f); // LDA #$008F
      // fall through
    case 0x81f225u: r64_sta_y(w,c,10); // STA $000A,Y
      // fall through
    case 0x81f228u: r64_lda(c,wram_r16(w,8)); // LDA $0008
      // fall through
    case 0x81f22bu: r64_sta_y(w,c,12); // STA $000C,Y
      // fall through
    case 0x81f22eu: r64_lda(c,0x0038); // LDA #$0038
      // fall through
    case 0x81f231u: r64_sta_y(w,c,14); // STA $000E,Y
      // fall through
    case 0x81f234u: r64_lda(c,0x8000); // LDA #$8000
      // fall through
    case 0x81f237u: r64_lda(c,(uint16_t)(c->a|wram_r16(w,c->y))); // ORA $0000,Y
      // fall through
    case 0x81f23au: r64_sta_y(w,c,0); // STA $0000,Y
      // fall through
    case 0x81f23du: r64_put_dp(w,c,0x3c,0); // STZ $3C
      // fall through
    case 0x81f23fu: r64_put_dp(w,c,0x1c,0); // STZ $1C
      // fall through
    case 0x81f241u: r64_lda(c,0x0040); // LDA #$0040
      // fall through
    case 0x81f244u: r64_put_dp(w,c,0x40,c->a); // STA $40
      // fall through
    case 0x81f246u: r64_lda(c,0x0064); // LDA #$0064
      // fall through
    case 0x81f249u: r64_put_dp(w,c,0x3e,c->a); // STA $3E
      c->pc=0x81f24bu;break; // next ROM LDA/JSL stays machine-owned
    default: break;
  }
}
