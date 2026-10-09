#include "native/runtime.c"
#include <stdio.h>
#include <assert.h>
static void frames(ZamnNativeRuntime*rt,int n,uint16_t pad){ZamnFrameResult fr;for(int i=0;i<n;++i){zamn_runtime_set_pad(rt,0,pad);assert(zamn_runtime_frame(rt,NULL,0,&fr));}}
static unsigned weapon(ZamnNativeRuntime*rt){return wram_r16((Wram*)rt->snes->ram,W_PLAYER_WEAPON);}
int main(int argc,char**argv){
 FILE*f=fopen(argv[1],"rb");assert(f);uint8_t*rom=malloc(1048576);assert(fread(rom,1,1048576,f)==1048576);fclose(f);
 ZamnNativeRuntime*rt=zamn_runtime_create(rom,1048576,ZAMN_RUNTIME_NATIVE_CUTOVER);assert(rt);free(rom);
 for(int i=0;i<3600;++i){frames(rt,1,0);if(zamn_runtime_title_ready(rt))break;}
 zamn_runtime_title_start(rt);for(int i=0;i<900;++i)frames(rt,1,(i<300 && i%60==0)?8:0);assert(zamn_runtime_player_stage_hud(rt));
 Wram*w=(Wram*)rt->snes->ram;unsigned base=rom_word(&rt->cosim.rom,PLAYER_INVENTORY_BASES);
 for(int i=0;i<14;++i)wram_w16(w,base+2*i,(i==0||i==2||i==5)?20:0);
 wram_w16(w,W_PLAYER_WEAPON,0);
 frames(rt,12,1);assert(weapon(rt)==2);frames(rt,4,0);
 frames(rt,4,0x1000);assert(weapon(rt)==0);
 frames(rt,20,0x1000);assert(weapon(rt)==0);frames(rt,4,0);
 frames(rt,4,0x1000);assert(weapon(rt)==5);frames(rt,4,0);
 frames(rt,4,1);assert(weapon(rt)==0);frames(rt,4,0);
 // A save made with a queued LT edge must replay identically.
 zamn_runtime_set_pad(rt,0,0x1000);int size=zamn_runtime_snapshot_size(rt),used=0;void*state=malloc(size);
 assert(zamn_runtime_save_snapshot(rt,state,size,&used));frames(rt,4,0x1000);uint64_t h=zamn_runtime_state_hash(rt);assert(weapon(rt)==5);
 assert(zamn_runtime_load_snapshot(rt,state,used));frames(rt,4,0x1000);assert(weapon(rt)==5);assert(h==zamn_runtime_state_hash(rt));
 puts("R79 actual-ROM runtime PASS: RT forward, LT reverse/wrap, held LT single step, save/replay hash");
}
