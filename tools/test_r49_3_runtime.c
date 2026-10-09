#include "native/runtime.c"
#include <stdio.h>
#include <assert.h>
static uint8_t pixels[512*480*4];
static void picture(ZamnNativeRuntime* rt,const char* name) {
  zamn_runtime_copy_legacy_frame(rt,pixels,512*4,NULL,0,480);
  FILE* f=fopen(name,"wb");fprintf(f,"P6\n512 480\n255\n");
  for(int i=0;i<512*480;++i){fputc(pixels[i*4+2],f);fputc(pixels[i*4+1],f);fputc(pixels[i*4],f);}fclose(f);
}
int main(int argc,char**argv) {
  assert(argc==2);FILE*f=fopen(argv[1],"rb");assert(f);
  uint8_t*rom=malloc(1048576);assert(fread(rom,1,1048576,f)==1048576);fclose(f);
  uint64_t hash=1469598103934665603ULL;
  for(int i=0;i<1048576;++i)hash=(hash^rom[i])*1099511628211ULL;
  assert(hash==0x985A7978F6E47187ULL);
  ZamnNativeRuntime*rt=zamn_runtime_create(rom,1048576,ZAMN_RUNTIME_NATIVE_CUTOVER);assert(rt);free(rom);
  ZamnFrameResult fr;int boot;
  for(boot=0;boot<3600;++boot) {
    assert(zamn_runtime_frame(rt,NULL,0,&fr));
    if(zamn_runtime_title_ready(rt))break;
  }
  printf("title detected at frame %d page %04x\n",boot,title_page(rt));fflush(stdout);
  assert(boot<3600);picture(rt,"title-original.ppm");
  for(int i=0;i<950;++i){zamn_runtime_title_control(rt,true,-1);assert(zamn_runtime_frame(rt,NULL,0,&fr));assert(zamn_runtime_title_ready(rt));}
  picture(rt,"title-hidden.ppm");
  // Two differently aged frontends must produce the exact same match state.
  assert(zamn_runtime_reset_to_title(rt,true));uint64_t first=zamn_runtime_state_hash(rt);
  for(int i=0;i<40;++i)assert(zamn_runtime_frame(rt,NULL,0,&fr));
  assert(zamn_runtime_reset_to_title(rt,true));assert(first==zamn_runtime_state_hash(rt));
  // R49.4 incremental preparation returns to the network pump every frame,
  // clears noPixels on every return, and reaches the same deterministic start.
  zamn_runtime_title_reset_begin(rt);
  int progress=0,pumps=0;
  while(progress==0){progress=zamn_runtime_title_reset_step(rt,true);++pumps;assert(!rt->snes->ppu->noPixels);}
  assert(progress==1 && pumps>2000 && pumps<3600);
  assert(first==zamn_runtime_state_hash(rt));
  printf("PASS incremental preparation: %d network-pump opportunities, identical start hash\n",pumps);
  // R49.5: cache the canonical title *before* START, then prove restoring that
  // snapshot plus one deterministic START frame is identical to the old full
  // neutral reboot path. This is the production instant-start invariant.
  assert(zamn_runtime_reset_to_title(rt,false));
  uint64_t cached_title_hash=zamn_runtime_state_hash(rt);
  int cacheCap=zamn_runtime_snapshot_size(rt),cacheUsed=0;uint8_t*cacheState=malloc(cacheCap);
  assert(cacheState && zamn_runtime_save_snapshot(rt,cacheState,cacheCap,&cacheUsed));
  assert(zamn_runtime_title_start(rt));
  uint64_t cached_start_hash=zamn_runtime_state_hash(rt);
  assert(cached_start_hash==first);
  for(int i=0;i<40;++i)assert(zamn_runtime_frame(rt,NULL,0,&fr));
  assert(zamn_runtime_load_snapshot(rt,cacheState,cacheUsed));
  assert(zamn_runtime_title_ready(rt));
  assert(zamn_runtime_state_hash(rt)==cached_title_hash);
  assert(zamn_runtime_title_start(rt));
  assert(zamn_runtime_state_hash(rt)==cached_start_hash);
  free(cacheState);
  puts("PASS R49.5 canonical-title snapshot restore + one-frame START equals cold prepare");
  // R49.1 APU continuation still survives a checkpoint and exact replay.
  int cap=zamn_runtime_snapshot_size(rt),used;uint8_t*state=malloc(cap);
  assert(zamn_runtime_save_snapshot(rt,state,cap,&used));
  uint32_t pending=rt->snes->apuMasterPending;int64_t debt=rt->snes->apuCycleDebtNumerator;
  assert(zamn_runtime_frame(rt,NULL,0,&fr));uint64_t next=zamn_runtime_state_hash(rt);
  assert(zamn_runtime_load_snapshot(rt,state,used));
  assert(pending==rt->snes->apuMasterPending && debt==rt->snes->apuCycleDebtNumerator);
  assert(zamn_runtime_frame(rt,NULL,0,&fr));assert(next==zamn_runtime_state_hash(rt));free(state);
  // PASSWORD delegates to the genuine ROM branch.
  assert(zamn_runtime_reset_to_title(rt,false));zamn_runtime_title_control(rt,false,1);
  zamn_runtime_set_pad(rt,0,1u<<3);
  for(int i=0;i<100;++i){assert(zamn_runtime_frame(rt,NULL,0,&fr));zamn_runtime_set_pad(rt,0,0);}
  assert(!zamn_runtime_title_ready(rt));picture(rt,"password.ppm");
  zamn_runtime_destroy(rt);puts("PASS ROM hash, title hold, second session canonical hash, APU replay, password exit");
}
