#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "statehandler.h"
#include "port/score.h"

void original_sh_handleByteArray(StateHandler*, uint8_t*, int);
void original_sh_handleWordArray(StateHandler*, uint16_t*, int);
static unsigned random_state = 0x12345678;
static unsigned next_random(void) {
  random_state ^= random_state << 13;
  random_state ^= random_state >> 17;
  random_state ^= random_state << 5;
  return random_state;
}
static void arrays(void) {
  static uint8_t a[300032], b[300032], x[270000], y[270000];
  static uint16_t wx[65536], wy[65536];
  const int sizes[] = {0, 1, 2, 3, 15, 256, 32768, 65536};
  for(int words=0; words<2; ++words)
  for(int saving=0; saving<2; ++saving)
  for(int offset=0; offset<4; ++offset)
  for(unsigned n=0; n<sizeof(sizes)/sizeof(sizes[0]); ++n)
  for(int tail=-3; tail<=3; ++tail) {
    int count=sizes[n], capacity=offset+count*(words?2:1)+tail;
    if(capacity<offset) capacity=offset;
    for(int i=0;i<(int)sizeof(a);++i) a[i]=(uint8_t)next_random();
    memcpy(b,a,sizeof(a));
    for(int i=0;i<(int)sizeof(x);++i) x[i]=(uint8_t)next_random();
    memcpy(y,x,sizeof(x));
    for(int i=0;i<65536;++i) wx[i]=(uint16_t)next_random();
    memcpy(wy,wx,sizeof(wx));
    StateHandler old={saving,offset,a,capacity,true,false};
    StateHandler now={saving,offset,b,capacity,true,false};
    if(words) {
      original_sh_handleWordArray(&old,wx,count);
      sh_handleWordArray(&now,wy,count);
    } else {
      original_sh_handleByteArray(&old,x,count);
      sh_handleByteArray(&now,y,count);
    }
    assert(old.offset==now.offset && old.failed==now.failed);
    assert(!memcmp(a,b,sizeof(a)) && !memcmp(x,y,sizeof(x)) && !memcmp(wx,wy,sizeof(wx)));
  }
  // Ordinary growable saves must retain their contents too.
  for(int words=0;words<2;++words) {
    StateHandler *old=sh_init(true,NULL,0), *now=sh_init(true,NULL,0);
    if(words) {
      original_sh_handleWordArray(old,wx,65536);
      sh_handleWordArray(now,wx,65536);
    } else {
      original_sh_handleByteArray(old,x,270000);
      sh_handleByteArray(now,x,270000);
    }
    assert(old->offset==now->offset && old->failed==now->failed);
    assert(!memcmp(old->data,now->data,old->offset));
    sh_free(old); sh_free(now);
  }
  puts("PASS snapshot arrays: byte-identical saves/loads, odd offsets, truncation, capacity edges, growable saves");
}
static void score_guard(void) {
  static Wram live, saved, scratch;
  static uint8_t rom_data[0x8000];
  Rom rom={rom_data,sizeof(rom_data)};
  unsigned slot_owner[]={0,2,4};
  for(int i=0;i<WRAM_SIZE;++i) live.bytes[i]=(uint8_t)next_random();
  for(unsigned a=0;a<3;++a) for(unsigned b=0;b<3;++b) {
    wram_w16(&live,W_SCORE_SLOT_SIDE,slot_owner[a]);
    wram_w16(&live,W_SCORE_SLOT_SIDE+2,slot_owner[b]);
    saved=live;
    // All possible table destinations, both player sides, all ownership cases.
    for(unsigned target=0;target<=65535;++target) {
      for(unsigned slot=0;slot<6;slot+=2) {
        unsigned at=(SCORE_ADD_TABLE&0x7fff)+slot;
        rom_data[at]=(uint8_t)target; rom_data[at+1]=(uint8_t)(target>>8);
      }
      for(int side=0;side<2;++side) {
        bool supported=score_add_supported(&live,&rom,side!=0);
        // The original operation writes only score words; restore those each time.
        memcpy(scratch.bytes+W_PLAYER_SCORE,live.bytes+W_PLAYER_SCORE,8);
        wram_w16(&scratch,W_SCORE_SLOT_SIDE,slot_owner[a]);
        wram_w16(&scratch,W_SCORE_SLOT_SIDE+2,slot_owner[b]);
        ScoreResult result;
        bool executed=score_add(&scratch,&rom,side!=0,(uint16_t)next_random(),(target&1)!=0,&result);
        assert(supported==executed);
      }
    }
    assert(!memcmp(&live,&saved,sizeof(live)));
  }
  // Mixed targets ensure the predicate selects the same slot, not any supported slot.
  const uint16_t targets[]={SCORE_ADD_SLOT0,SCORE_ADD_SLOT1,SCORE_ADD_DISCARD,0};
  for(int trial=0;trial<1000;++trial) {
    wram_w16(&live,W_SCORE_SLOT_SIDE,slot_owner[next_random()%3]);
    wram_w16(&live,W_SCORE_SLOT_SIDE+2,slot_owner[next_random()%3]);
    for(unsigned slot=0;slot<6;slot+=2) {
      unsigned at=(SCORE_ADD_TABLE&0x7fff)+slot; uint16_t t=targets[next_random()%4];
      rom_data[at]=(uint8_t)t; rom_data[at+1]=(uint8_t)(t>>8);
    }
    for(int side=0;side<2;++side) {
      scratch=live; ScoreResult result;
      assert(score_add_supported(&live,&rom,side!=0)==score_add(&scratch,&rom,side!=0,0x9999,true,&result));
    }
  }
  puts("PASS native score guard: every 16-bit target, both players, ownership swaps/missing/duplicates, mixed tables, no WRAM mutation");
}
static double benchmark(int original,int saving) {
  static uint8_t state[270000], ram[131072], apu[65536];
  static uint16_t vram[32768];
  const int iterations=2000;
  clock_t start=clock();
  for(int i=0;i<iterations;++i) {
    StateHandler sh={saving,1,state,sizeof(state),true,false};
    ram[0]=(uint8_t)i;
    if(original) {
      original_sh_handleByteArray(&sh,ram,sizeof(ram));
      original_sh_handleByteArray(&sh,apu,sizeof(apu));
      original_sh_handleWordArray(&sh,vram,32768);
    } else {
      sh_handleByteArray(&sh,ram,sizeof(ram));
      sh_handleByteArray(&sh,apu,sizeof(apu));
      sh_handleWordArray(&sh,vram,32768);
    }
    assert(!sh.failed && sh.offset==262145);
  }
  return 1000.0*(clock()-start)/CLOCKS_PER_SEC/iterations;
}
int main(void) {
  arrays(); score_guard();
  for(int saving=0;saving<2;++saving) {
    double old=benchmark(1,saving), now=benchmark(0,saving);
    printf("HOST ONLY %s 256 KiB arrays: original %.4f ms, native %.4f ms per iteration\n",saving?"save":"load",old,now);
  }
  return 0;
}
