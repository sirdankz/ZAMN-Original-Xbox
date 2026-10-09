// R54 exact-state differential of 18 actor/state-machine native boundaries.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "port/hotpaths.h"
#include "cpu.h"
static uint8_t rom_data[1048576];
static Wram refw,natw;
static Cpu* active_cpu; static int active_sec,active_trial;
static unsigned seed=0x829df1;
static unsigned rnd(void){ seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;return seed; }
static uint8_t rd(void*unused,uint32_t a){(void)unused;unsigned b=a>>16,o=a&65535;
 if(b==0x7e||b==0x7f)return wram_r8(&refw,a-0x7e0000);
 if(!(b&0x40)&&o<0x2000)return wram_r8(&refw,o);
 if(o<0x8000){fprintf(stderr,"INVALID RD %06x sec=%d trial=%d pc=%02x:%04x\n",a,active_sec,active_trial,active_cpu?active_cpu->k:0,active_cpu?active_cpu->pc:0);exit(8);}unsigned i=(b&127)*0x8000+(o&0x7fff);assert(i<sizeof rom_data);return rom_data[i];}
static void wr(void*unused,uint32_t a,uint8_t v){(void)unused;unsigned b=a>>16,o=a&65535;
 if(b==0x7e||b==0x7f) wram_w8(&refw,a-0x7e0000,v);
 else {if((b&0x40)||o>=0x2000){fprintf(stderr,"INVALID WR %06x k=%02x pc=%04x sec=%d trial=%d\n",a,active_cpu?active_cpu->k:0,active_cpu?active_cpu->pc:0,active_sec,active_trial);exit(7);}wram_w8(&refw,o,v);}}
static void idle(void*u,bool w){(void)u;(void)w;}
static uint8_t status(const Cpu*c){return c->n*128+c->v*64+c->mf*32+c->xf*16+c->d*8+c->i*4+c->z*2+c->c;}
static void cpu_load(Cpu*cpu,const PortCpu*c){cpu_reset(cpu,true);cpu->resetWanted=false;cpu->e=false;
 cpu->a=c->a;cpu->x=c->x;cpu->y=c->y;cpu->dp=c->d;cpu->sp=c->s;
 cpu->db=c->db;cpu->k=(uint8_t)(c->pc>>16);cpu->pc=(uint16_t)c->pc;
 cpu->n=!!(c->p&128);cpu->v=!!(c->p&64);cpu->i=!!(c->p&4);
 cpu->z=!!(c->p&2);cpu->c=!!(c->p&1);cpu->mf=cpu->xf=cpu->d=false;}
static const uint32_t entries[]={0x839843,0x83984f,0x83985d,0x839865,0x839880,0x83989b,0x8398aa,0x8398d2,0x8398f1,0x82988d,0x829890,0x8298b0,0x8298ce,0x82991a,0x829927,0x82993b,0x82993f};
static const uint32_t expected_exits[17][3]={
 {0x839848,0x839865},
 {0x83985d,0x839864},
 {0x839848,0x839865},
 {0x83987d},
 {0x839897},
 {0x8398aa,0x839916},
 {0x8398d2,0x839894},
 {0x8398e6},
 {0x839894,0x839916,0x839908},
 {0x829890},
 {0x8298b0},
 {0x8298ce},
 {0x8298e0},
 {0x829924},
 {0x82992c},
 {0x829927,0x82993f},
 {0x829969,0x829946},
};
static int exit_count[17][3];
static int checks=0;
static void compare(Cpu*cpu,PortCpu initial,Rom*rom,int which,int trial){
 active_cpu=cpu;active_sec=which;active_trial=trial;PortCpu c=initial;natw=refw;
 if(!native_r55_supported(&natw,rom,&c)) {
   printf("invalid setup sec=%d t=%d pc=%06x A=%04x X=%04x Y=%04x D=%04x DB=%02x\n",which,trial,c.pc,c.a,c.x,c.y,c.d,c.db);exit(3);
 }
 native_r55_actor_fragments(&natw,rom,&c,NULL);cpu_load(cpu,&initial);
 int steps=0;while((((unsigned)cpu->k<<16)|cpu->pc)!=c.pc&&steps++<150)cpu_runOpcode(cpu);
 if(steps>=150||(((unsigned)cpu->k<<16)|cpu->pc)!=c.pc||cpu->a!=c.a||cpu->x!=c.x||cpu->y!=c.y||cpu->dp!=c.d||cpu->sp!=c.s||cpu->db!=c.db||status(cpu)!=c.p||memcmp(&refw,&natw,sizeof refw)) {
   printf("FAIL section=%d t=%d pc=%06x reached=%02x:%04x wanted=%06x A=%04x/%04x X=%04x/%04x Y=%04x/%04x D=%04x/%04x S=%04x/%04x P=%02x/%02x steps=%d\n",
    which,trial,initial.pc,cpu->k,cpu->pc,c.pc,cpu->a,c.a,cpu->x,c.x,cpu->y,c.y,cpu->dp,c.d,cpu->sp,c.s,status(cpu),c.p,steps);
   for(unsigned i=0;i<WRAM_SIZE;i++)if(refw.bytes[i]!=natw.bytes[i]){printf("WRAM %05x ref=%02x native=%02x\n",i,refw.bytes[i],natw.bytes[i]);break;}exit(1);
 }
 int slot=-1;for(int k=0;k<3;k++) if(expected_exits[which][k]&&c.pc==expected_exits[which][k]) {slot=k;break;}
 if(slot<0){printf("unexpected exit %06x for %06x\n",c.pc,initial.pc);exit(1);}
 exit_count[which][slot]++;
 checks++;
}
int main(int argc,char**argv){assert(argc==2);FILE*f=fopen(argv[1],"rb");assert(f);
 assert(fread(rom_data,1,sizeof rom_data,f)==sizeof rom_data);fclose(f);
 uint64_t hash=UINT64_C(1469598103934665603);for(unsigned i=0;i<sizeof rom_data;i++)hash=(hash^rom_data[i])*UINT64_C(1099511628211);
 assert(hash==UINT64_C(0x985A7978F6E47187));Rom rom={rom_data,sizeof rom_data};Cpu*cpu=cpu_init(NULL,rd,wr,idle);
 for(int sec=0;sec<(int)(sizeof entries/sizeof entries[0]);sec++) {
  for(int t=0;t<1400;t++) {
   for(unsigned i=0;i<WRAM_SIZE;i++)refw.bytes[i]=(uint8_t)rnd();
   PortCpu c={(uint16_t)rnd(),(uint16_t)(rnd()%256),(uint16_t)(0x100+(rnd()%0x100)),
     (uint16_t)(0x180+(rnd()%8)*0x100),(uint16_t)(0x120+rnd()%0x100),
     (uint8_t)((t%4==0)?0x83:(t%4==1)?0x03:(t%4==2)?0x80:0x82),
     (uint8_t)(rnd()&0xc7),entries[sec]};
   if(sec==0 || sec==2) wram_w16(&refw,0x1ff6,t%2?0:(uint16_t)(rnd()|1));
   if(sec==1) {
     uint16_t ix=(uint16_t)(rnd()%0x500);wram_w16(&refw,c.d+6,ix);
     wram_w16(&refw,0x605a+ix,t%2?0x0080:(uint16_t)(rnd()&0x007f));
   }
   if(sec==4 || sec==6 || sec==7 || sec==8) {
     uint16_t iy=(uint16_t)(rnd()%0x1d00);wram_w16(&refw,c.d+8,iy);
   }
   if(sec==6) {
     uint16_t iy=wram_r16(&refw,c.d+8),delta=(uint16_t)rnd();
     wram_w16(&refw,c.d+0x28,delta);
     wram_w16(&refw,iy+4,t%2?(uint16_t)(0-delta):rnd());
     wram_w16(&refw,c.d+0x2a,t%3?1:(uint16_t)(rnd()|1));
     wram_w16(&refw,c.d+0x10,(uint16_t)(rnd()%12));
   }
   if(sec==5) {
     wram_w16(&refw,c.d+0x1e,(uint16_t)(t%3==0?0:t%3==1?0x8001:0xffff));
     wram_w16(&refw,0x1fb8,t%2?0:1);wram_w16(&refw,0x1fba,0);
   }
   if(sec==8) wram_w16(&refw,c.d+0x1e,(uint16_t)(t%4==0?0:t%4==1?0x8001:t%4==2?1:2));
   if(sec==9) c.a=(uint16_t)(rnd()%0x1f00);
   if(sec==10 || sec==11) c.y=(uint16_t)(rnd()%0x1d00);
   if(sec==15) wram_w16(&refw,c.d+0x0a,t%2?0:3);
   if(sec==16) wram_w16(&refw,c.d+0x20,t%2?0:3);
   compare(cpu,c,&rom,sec,t);
  }
  printf("section %06x passed 1400 full-state ROM/native comparisons exits=%d/%d\n",entries[sec],exit_count[sec][0],exit_count[sec][1]);
  if(expected_exits[sec][1]) assert(exit_count[sec][0]>0 && exit_count[sec][1]>0);
  if(expected_exits[sec][2]) assert(exit_count[sec][2]>0);
 }
 Wram orig=refw;PortCpu bad={0};bad.pc=entries[0];bad.db=0x82;bad.d=0x100;bad.s=0x110;bad.p=PORT_P_M;
 assert(!native_r55_supported(&refw,&rom,&bad));bad.p=0;bad.db=0x7e;assert(!native_r55_supported(&refw,&rom,&bad));
 bad.db=0x82;bad.d=0x1f00;assert(!native_r55_supported(&refw,&rom,&bad));
 assert(!memcmp(&refw,&orig,sizeof orig));
 cpu_free(cpu);printf("PASS %d R55 ROM/native full 128KiB+register+stack+flags comparisons\n",checks);
}
