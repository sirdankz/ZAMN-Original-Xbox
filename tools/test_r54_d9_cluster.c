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
 assert(o>=0x8000);unsigned i=(b&127)*0x8000+(o&0x7fff);assert(i<sizeof rom_data);return rom_data[i];}
static void wr(void*unused,uint32_t a,uint8_t v){(void)unused;unsigned b=a>>16,o=a&65535;
 if(b==0x7e||b==0x7f) wram_w8(&refw,a-0x7e0000,v);
 else {if((b&0x40)||o>=0x2000){fprintf(stderr,"INVALID WR %06x k=%02x pc=%04x sec=%d trial=%d\n",a,active_cpu?active_cpu->k:0,active_cpu?active_cpu->pc:0,active_sec,active_trial);exit(7);}wram_w8(&refw,o,v);}}
static void idle(void*u,bool w){(void)u;(void)w;}
static uint8_t status(const Cpu*c){return c->n*128+c->v*64+c->mf*32+c->xf*16+c->d*8+c->i*4+c->z*2+c->c;}
static void cpu_load(Cpu*cpu,const PortCpu*c){cpu_reset(cpu,true);cpu->resetWanted=false;cpu->e=false;
 cpu->a=c->a;cpu->x=c->x;cpu->y=c->y;cpu->dp=c->d;cpu->sp=c->s;
 cpu->db=c->db;cpu->k=0x82;cpu->pc=(uint16_t)c->pc;
 cpu->n=!!(c->p&128);cpu->v=!!(c->p&64);cpu->i=!!(c->p&4);
 cpu->z=!!(c->p&2);cpu->c=!!(c->p&1);cpu->mf=cpu->xf=cpu->d=false;}
static const uint32_t entries[]={0x82d8db,0x82d8fd,0x82d907,0x82d913,
 0x82d91d,0x82d92a,0x82d938,0x82d945,0x82d94d,0x82d96e,
 0x82d989,0x82d999,0x82d9ae,0x82d9b4,0x82d9c3,0x82d9d5,
 0x82d9dc,0x82d9eb};
static int checks=0;
// Each two-way ROM branch must exercise both exits in randomized testing.
static const uint32_t expected_exits[18][2]={
 {0x82d8ef,0x82d8f3},{0x82d907,0x82d9b4},{0x82d910,0x82d913},
 {0x82d91d,0x82d92a},{0x82d8f6,0},{0x82d938,0},
 {0x82d919,0x82d945},{0x82d94d,0},{0x82d919,0x82d96e},
 {0x82d919,0x82d989},{0x82d999,0},{0x82d9ae,0},
 {0x82d8f6,0},{0x82d9bf,0},{0x82d9d1,0},
 {0x82d9d8,0},{0x82d9e6,0x82d9eb},{0x82d9f1,0}
};
static int exit_count[18][2];
static void compare(Cpu*cpu,PortCpu initial,Rom*rom,int which,int trial){
 active_cpu=cpu;active_sec=which;active_trial=trial;PortCpu c=initial;natw=refw;
 if(!native_r54_supported(&natw,rom,&c)) {
   printf("invalid setup sec=%d t=%d pc=%06x A=%04x X=%04x Y=%04x D=%04x DB=%02x\n",which,trial,c.pc,c.a,c.x,c.y,c.d,c.db);exit(3);
 }
 native_r54_d9_cluster(&natw,rom,&c,NULL);cpu_load(cpu,&initial);
 int steps=0;while((((unsigned)cpu->k<<16)|cpu->pc)!=c.pc&&steps++<150)cpu_runOpcode(cpu);
 if(steps>=150||(((unsigned)cpu->k<<16)|cpu->pc)!=c.pc||cpu->a!=c.a||cpu->x!=c.x||cpu->y!=c.y||cpu->dp!=c.d||cpu->sp!=c.s||cpu->db!=c.db||status(cpu)!=c.p||memcmp(&refw,&natw,sizeof refw)) {
   printf("FAIL section=%d t=%d pc=%06x reached=%02x:%04x wanted=%06x A=%04x/%04x X=%04x/%04x Y=%04x/%04x D=%04x/%04x S=%04x/%04x P=%02x/%02x steps=%d\n",
    which,trial,initial.pc,cpu->k,cpu->pc,c.pc,cpu->a,c.a,cpu->x,c.x,cpu->y,c.y,cpu->dp,c.d,cpu->sp,c.s,status(cpu),c.p,steps);
   for(unsigned i=0;i<WRAM_SIZE;i++)if(refw.bytes[i]!=natw.bytes[i]){printf("WRAM %05x ref=%02x native=%02x\n",i,refw.bytes[i],natw.bytes[i]);break;}exit(1);
 }
 int slot=(c.pc==expected_exits[which][0])?0:(c.pc==expected_exits[which][1])?1:-1;
 if(slot<0){printf("unexpected exit %06x for %06x\n",c.pc,initial.pc);exit(1);}
 exit_count[which][slot]++;
 checks++;
}
int main(int argc,char**argv){assert(argc==2);FILE*f=fopen(argv[1],"rb");assert(f);
 assert(fread(rom_data,1,sizeof rom_data,f)==sizeof rom_data);fclose(f);
 uint64_t hash=UINT64_C(1469598103934665603);for(unsigned i=0;i<sizeof rom_data;i++)hash=(hash^rom_data[i])*UINT64_C(1099511628211);
 assert(hash==UINT64_C(0x985A7978F6E47187));Rom rom={rom_data,sizeof rom_data};Cpu*cpu=cpu_init(NULL,rd,wr,idle);
 for(int sec=0;sec<(int)(sizeof entries/sizeof entries[0]);sec++) {
  for(int t=0;t<1500;t++) {
   for(unsigned i=0;i<WRAM_SIZE;i++)refw.bytes[i]=(uint8_t)rnd();
   PortCpu c={(uint16_t)rnd(),(uint16_t)(rnd()%128),(uint16_t)(rnd()%128),
     (uint16_t)(0x180+(rnd()%8)*0x100),(uint16_t)(0x120+rnd()%0x100),
     (uint8_t)((t%4==0)?0x82:(t%4==1)?0x02:(t%4==2)?0x80:0x83),
     (uint8_t)(rnd()&0xc7),entries[sec]};
   wram_w16(&refw,c.d+0,sec==0?(uint16_t)(rnd()%33):(uint16_t)(rnd()%16));
   wram_w16(&refw,c.d+0x1c,(uint16_t)(rnd()%80));
   wram_w16(&refw,c.d+0x08,(uint16_t)(rnd()%0x1e80));
   if(sec==0){wram_w16(&refw,0x20,(uint16_t)(t%2?(wram_r16(&refw,c.d+0)&1):(rnd()%2)));}
   if(sec==1){uint16_t idx=wram_r16(&refw,c.d+0x1c);wram_w16(&refw,0x1f98+idx,t%2?0:(uint16_t)(rnd()|1u));}
   if(sec==2){wram_w16(&refw,c.d+0x1e,(uint16_t)(rnd()%4));wram_w16(&refw,0x1d52,t%2?wram_r16(&refw,c.d+0x1e):5);}
   if(sec==3){wram_w16(&refw,0x6e30,t%2?0:0x0010);}
   if(sec==5){wram_w16(&refw,c.d+0x0a,t%2?0:0xffff);wram_w16(&refw,0x6e30,t%3?0x16:0);}
   if(sec==6){c.a=(uint16_t)(1+rnd()%64);wram_w16(&refw,0x605a+c.a-1,t%2?0x80:0);}
   if(sec==8){c.x=(uint16_t)(rnd()%128);uint16_t idx=(uint16_t)(rnd()%0x40);wram_w16(&refw,c.d+0x1c,idx);wram_w16(&refw,0xd2+idx,0x1500+(rnd()%0x120));
     uint16_t y=wram_r16(&refw,0xd2+idx);wram_w16(&refw,2+y,(uint16_t)(rnd()%0x1ff));wram_w16(&refw,0x6df4+c.x,(uint16_t)(rnd()%0x1ff));}
   if(sec==9){c.x=(uint16_t)(rnd()%128);c.y=(uint16_t)(rnd()%0x100);
     wram_w16(&refw,6+c.y,(uint16_t)(rnd()%0x1ff));wram_w16(&refw,0x6df6+c.x,(uint16_t)(rnd()%0x1ff));}
   if(sec==11){wram_w16(&refw,c.d+0x08,(uint16_t)(rnd()%0x1e80));}
   if(sec==13){wram_w16(&refw,c.d+0x1c,(uint16_t)(rnd()%0x60));}
   if(sec==16){wram_w16(&refw,c.d+0,t%2?0:(uint16_t)(rnd()|1u));}
   compare(cpu,c,&rom,sec,t);
  }
  printf("section %06x passed 1500 full-state ROM/native comparisons exits=%d/%d\n",entries[sec],exit_count[sec][0],exit_count[sec][1]);
  if(expected_exits[sec][1]) assert(exit_count[sec][0]>0 && exit_count[sec][1]>0);
 }
 Wram orig=refw;PortCpu bad={0};bad.pc=entries[0];bad.db=0x82;bad.d=0x100;bad.s=0x110;bad.p=PORT_P_M;
 assert(!native_r54_supported(&refw,&rom,&bad));bad.p=0;bad.db=0x7e;assert(!native_r54_supported(&refw,&rom,&bad));
 bad.db=0x82;bad.d=0x1f00;assert(!native_r54_supported(&refw,&rom,&bad));
 assert(!memcmp(&refw,&orig,sizeof orig));
 cpu_free(cpu);printf("PASS %d R54 ROM/native full 128KiB+register+stack+flags comparisons\n",checks);
}
