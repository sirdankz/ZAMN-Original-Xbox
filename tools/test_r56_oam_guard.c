// gcc/clang -std=c11 -O2 -ffunction-sections -fdata-sections -Isrc \
//   tools/test_r56_oam_guard.c src/port/oam.c src/assets/rom.c \
//   src/assets/sprite.c -Wl,--gc-sections -o /tmp/test_r56_oam
// /tmp/test_r56_oam /path/to/owned/USA/zamn.sfc
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "port/oam.h"
#include "port/thread.h"

// Fail all collision callbacks. Approval is valid only if neither the real
// preflight nor this test shim can reach a callback. This is stricter than the
// live game, where some handlers are natively supported.
static int handler_calls;
bool thread_call_handler_counted(Wram* w, const Rom* r, uint16_t slot,
                                 uint16_t arg, bool carry,
                                 ThreadCallResult* out, ThreadCallWork* work) {
  (void)w; (void)r; (void)slot; (void)arg; (void)carry; (void)out; (void)work;
  handler_calls++;
  return false;
}
static Wram live, before, scratch;
static uint32_t rng=0x56bd1f1u;
static uint32_t rnd(void) {
  rng ^= rng<<13; rng ^= rng>>17; rng ^= rng<<5; return rng;
}
int main(int argc, char** argv) {
  if(argc!=2) return fprintf(stderr,"need reference ROM argument\n"),2;
  FILE* f=fopen(argv[1],"rb"); if(!f) return 2;
  uint8_t* romdata=(uint8_t*)malloc(1048576); assert(romdata);
  assert(fread(romdata,1,1048576,f)==1048576); fclose(f);
  Rom rom={romdata,1048576};
  uint32_t approved=0, deferred=0, original_approved=0;
  for(int t=0;t<24000;t++) {
    // R56 preflight may read all 128KiB; randomize unrelated state too.
    for(size_t i=0;i<WRAM_SIZE;i++) live.bytes[i]=(uint8_t)rnd();
    int n=t%33;
    const uint16_t cx=(uint16_t)rnd(), cy=(uint16_t)rnd();
    wram_w16(&live,W_CAMERA_X,cx);wram_w16(&live,W_CAMERA_Y,cy);
    uint16_t ord[32];
    for(int j=0;j<n;j++)ord[j]=(uint16_t)(W_ACTOR_SLOTS+ACTOR_SLOT_STRIDE*j);
    for(int j=n-1;j>0;j--){int k=(int)(rnd()%(j+1));uint16_t v=ord[j];ord[j]=ord[k];ord[k]=v;}
    for(int j=0;j<n;j++) {
      uint16_t rec=ord[j];
      wram_w16(&live,rec+ACTOR_NEXT,j+1<n?ord[j+1]:0);
      uint16_t flags=(rnd()%5?ACTOR_DRAW:0) | (rnd()%5==0?ACTOR_SCREEN_SPACE:0) |
                     (rnd()%2?ACTOR_SORT_FIRST:0);
      wram_w16(&live,rec+ACTOR_FLAGS,flags);
      // Vary world and screen locations, with overlapping pairs in 1/3 of cases.
      uint16_t x=(t%3==0)?cx:(t%3==1)?(uint16_t)(cx+j*30):(uint16_t)(cx+(rnd()%600));
      uint16_t y=(t%3==0)?cy:(t%3==1)?(uint16_t)(cy+j*30):(uint16_t)(cy+(rnd()%600));
      wram_w16(&live,rec+ACTOR_X,x);wram_w16(&live,rec+ACTOR_Y,y);
      wram_w16(&live,rec+ACTOR_COLLIDE_ID,(rnd()%5==0)?0:(uint16_t)(1+rnd()%8));
      int meta_case=(int)(rnd()%8);
      uint16_t bank=meta_case<4?0x8f:(meta_case==4?0x90:0x7e);
      uint16_t ptr=meta_case==0?0xfffe:meta_case==1?0xff80:(uint16_t)(0x8000+rnd()%0x7c00);
      wram_w16(&live,rec+ACTOR_META_BANK,bank);wram_w16(&live,rec+ACTOR_META,ptr);
    }
    wram_w16(&live,W_ACTOR_LIST_HEAD,n?ord[0]:0);
    if(t%79==2 && n>=2) wram_w16(&live,ord[n-1]+ACTOR_NEXT,ord[0]);
    if(t%83==3 && n>=1) wram_w16(&live,W_ACTOR_LIST_HEAD,0x1555);
    before=live;
    bool quick=sprite_build_oam_readonly_safe(&live,&rom);
    if(memcmp(&live,&before,sizeof live)!=0) { fprintf(stderr,"mutated t=%d\n",t);return 1; }
    if(quick) {
      approved++;
      scratch=live; handler_calls=0;
      bool baseline=sprite_build_oam_supported_preflight(&scratch,&rom,0);
      if(!baseline || handler_calls) {
        fprintf(stderr,"FALSE APPROVAL: iteration=%d n=%d old=%d callbacks=%d\n",
                t,n,(int)baseline,handler_calls);return 1;
      }
      original_approved++;
    } else deferred++;
  }
  printf("R56 OAM proof PASS cases=24000 approved=%u deferred=%u baseline-approved=%u bytes-avoided=%llu\n",
     approved,deferred,original_approved,(unsigned long long)approved*65536u);
  free(romdata);return 0;
}
