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

// Model exactly the no-handler fast exit. Any nonempty handler declines,
// which safely tests all R57 approvals and unsupported-dispatch rejections.
static int handler_calls;
bool thread_call_handler_counted(Wram* w, const Rom* r, uint16_t slot,
                                 uint16_t arg, bool carry,
                                 ThreadCallResult* out, ThreadCallWork* work) {
  (void)r; (void)arg; (void)carry; (void)work;
  handler_calls++;
  uint16_t lo = wram_r16(w, W_THREAD_HANDLER + slot);
  uint16_t bank = wram_r16(w, W_THREAD_HANDLER_BANK + slot);
  out->entered = false;
  if ((lo | bank) == 0) return true;
  uint32_t entry=((uint32_t)(bank&255u)<<16)|lo;
  if(entry==SHOT_EDAA_COLLIDE_ENTRY || entry==ACTOR_845E_COLLIDE_ENTRY ||
     entry==SHOT_F6A3_COLLIDE_ENTRY || entry==ENEMY_CDDE_COLLIDE_ENTRY ||
     entry==ENEMY_B592_COLLIDE_ENTRY) {
    // The five native routines are total. The two approved as *first*
    // callbacks never touch actor records or handler registrations. Parking
    // may change WAIT only and is immaterial to subsequent registrations.
    out->entered=true;
    if(entry==ACTOR_845E_COLLIDE_ENTRY) {
      if((arg & 0x7fff)>0x005e) wram_w16(w,W_THREAD_WAIT+slot,0x8000);
    }
    return true;
  }
  return false;
}
static Wram live, before, scratch;
static uint32_t rng=0x57bd1f1u;
static uint32_t rnd(void) {
  rng ^= rng<<13; rng ^= rng>>17; rng ^= rng<<5; return rng;
}
int main(int argc, char** argv) {
  if(argc!=2) return fprintf(stderr,"need reference ROM argument\n"),2;
  FILE* f=fopen(argv[1],"rb"); if(!f) return 2;
  uint8_t* romdata=(uint8_t*)malloc(1048576); assert(romdata);
  assert(fread(romdata,1,1048576,f)==1048576); fclose(f);
  Rom rom={romdata,1048576};
  uint32_t approved=0, rejected=0, deferred=0, original_approved=0, original_rejected=0, r56approved=0, r58approved=0;
  for(int t=0;t<16000;t++) {
    // R56 preflight may read all 128KiB; randomize unrelated state too.
    for(size_t i=0;i<WRAM_SIZE;i++) live.bytes[i]=(uint8_t)rnd();
    int n=t%33;
    // Initialize all 24 handler slots; the short and long decision paths must
    // see the same state as thread_call_handler_counted.
    for (int slot=0;slot<48;slot+=2) {
      int type=(int)(rnd()%7);
      static const uint32_t candidate[] = {0,0,0, SHOT_EDAA_COLLIDE_ENTRY,
          ACTOR_845E_COLLIDE_ENTRY, SHOT_F6A3_COLLIDE_ENTRY,
          ENEMY_CDDE_COLLIDE_ENTRY, ENEMY_B592_COLLIDE_ENTRY,
          PLAYER_COLLIDE_ENTRY, ENEMY_COLLIDE_ENTRY, 0x7aabcd,0x7acafe};
      uint32_t entry = candidate[rnd() % (sizeof candidate/sizeof candidate[0])];
      wram_w16(&live,W_THREAD_HANDLER+slot,(uint16_t)entry);
      wram_w16(&live,W_THREAD_HANDLER_BANK+slot,(uint16_t)(entry>>16));
    }
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
      wram_w16(&live,rec+ACTOR_THREAD,(uint16_t)((rnd()%24)*2));
      int meta_case=(int)(rnd()%8);
      uint16_t bank=meta_case<4?0x8f:(meta_case==4?0x90:0x7e);
      uint16_t ptr=meta_case==0?0xfffe:meta_case==1?0xff80:(uint16_t)(0x8000+rnd()%0x7c00);
      wram_w16(&live,rec+ACTOR_META_BANK,bank);wram_w16(&live,rec+ACTOR_META,ptr);
    }
    wram_w16(&live,W_ACTOR_LIST_HEAD,n?ord[0]:0);
    if(t%79==2 && n>=2) wram_w16(&live,ord[n-1]+ACTOR_NEXT,ord[0]);
    if(t%83==3 && n>=1) wram_w16(&live,W_ACTOR_LIST_HEAD,0x1555);
    before=live;
    int decision=sprite_build_oam_r57_fast_decision(&live,&rom);
    bool prev=sprite_build_oam_readonly_safe(&live,&rom);
    if(memcmp(&live,&before,sizeof live)!=0) { fprintf(stderr,"mutated t=%d\n",t);return 1; }
    if (prev) { r56approved++; if(decision!=1) {
      fprintf(stderr,"R56 approval lost t=%d n=%d decision=%d\n",t,n,decision);return 1;
    }}
    if (decision!=0) {
      scratch=live; handler_calls=0;
      bool baseline=sprite_build_oam_supported_preflight(&scratch,&rom,0);
      if (decision==1 || decision==4) {
        if(decision==4)r58approved++;
        approved++;
        if(!baseline) {
          fprintf(stderr,"FALSE APPROVAL: iteration=%d n=%d calls=%d\n",t,n,handler_calls);return 1;
        }
        original_approved++;
      } else if (decision==2) {
        rejected++;
        if(baseline) {
          fprintf(stderr,"FALSE REJECTION: iteration=%d n=%d calls=%d\n",t,n,handler_calls);return 1;
        }
        original_rejected++;
      } else return 1;
    } else deferred++;
  }
  printf("R58 OAM decision PASS cases=16000 approved=%u NEW-nonmutating=%u rejected=%u deferred=%u R56-approved=%u saved-bytes=%llu\n",
     approved,r58approved,rejected,deferred,r56approved,(unsigned long long)(approved+rejected)*65536u);
  free(romdata);return 0;
}
