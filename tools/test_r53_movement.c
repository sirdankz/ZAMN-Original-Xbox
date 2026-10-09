// R53 randomized, exact state differential against the USA ROM / LakeSnes.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "port/hotpaths.h"
#include "cpu.h"
static uint8_t rom_data[1048576];
static Wram refw,natw;
static unsigned seed=0x8297d0;
static unsigned rnd(void){seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;return seed;}
static uint8_t rd(void*unused,uint32_t a){(void)unused;unsigned b=a>>16,o=a&65535;
 if(b==0x7e||b==0x7f)return wram_r8(&refw,a-0x7e0000);
 if(!(b&0x40)&&o<0x2000)return wram_r8(&refw,o);
 if(o<0x8000)return 0;
 unsigned i=(b&127)*0x8000+(o&0x7fff);return i<sizeof(rom_data)?rom_data[i]:0;}
static void wr(void*unused,uint32_t a,uint8_t v){(void)unused;unsigned b=a>>16,o=a&65535;
 if(b==0x7e||b==0x7f)wram_w8(&refw,a-0x7e0000,v);
 else {assert(!(b&0x40)&&o<0x2000);wram_w8(&refw,o,v);} }
static void idle(void*u,bool w){(void)u;(void)w;}
static uint8_t status(const Cpu*c){return c->n*128+c->v*64+c->mf*32+c->xf*16+c->d*8+c->i*4+c->z*2+c->c;}
static void cpu_load(Cpu*cpu,const PortCpu*c){cpu_reset(cpu,true);cpu->resetWanted=false;cpu->e=false;cpu->a=c->a;cpu->x=c->x;cpu->y=c->y;cpu->dp=c->d;cpu->sp=c->s;cpu->db=c->db;cpu->k=0x82;cpu->pc=(uint16_t)c->pc;cpu->n=!!(c->p&128);cpu->v=!!(c->p&64);cpu->i=!!(c->p&4);cpu->z=!!(c->p&2);cpu->c=!!(c->p&1);cpu->mf=cpu->xf=cpu->d=false;}
static unsigned exits[24][4];
static const uint32_t entries[]={0x829715,0x82973e,0x82974a,0x829751,0x829759,0x829765,
0x829773,0x82977f,0x8297be,0x8297d4,0x8297e0,0x8297e9,0x829800,0x82980e,
0x82981a,0x829831,0x82983f,0x82985b,0x829863,0x82986b,0x8298ea,0x8298fe,0x829994,0x8299cb};
static void compare(Cpu*cpu,PortCpu initial,Rom*rom,int which,int trial){
 PortCpu c=initial;natw=refw;
 if(!native_r53_supported(&natw,rom,&c)){printf("unsupported i=%d t=%d pc=%06x\n",which,trial,c.pc);exit(3);}
 native_r53_movement(&natw,rom,&c,NULL);cpu_load(cpu,&initial);
 int steps=0;while((((unsigned)cpu->k<<16)|cpu->pc)!=c.pc&&steps++<110) {
  cpu_runOpcode(cpu);
}
 if(steps>=110||(((unsigned)cpu->k<<16)|cpu->pc)!=c.pc||cpu->a!=c.a||cpu->x!=c.x||cpu->y!=c.y||cpu->dp!=c.d||cpu->sp!=c.s||cpu->db!=c.db||status(cpu)!=c.p||memcmp(&refw,&natw,sizeof(refw))){
  printf("FAIL sec=%d t=%d entry=%06x at=%06x native=%06x A=%04x/%04x X=%04x/%04x Y=%04x/%04x D=%04x/%04x DB=%02x/%02x S=%04x/%04x P=%02x/%02x steps=%d\n",which,trial,(unsigned)initial.pc,(unsigned)(((unsigned)cpu->k<<16)|cpu->pc),(unsigned)c.pc,cpu->a,c.a,cpu->x,c.x,cpu->y,c.y,cpu->dp,c.d,cpu->db,c.db,cpu->sp,c.s,status(cpu),c.p,steps);
  for(unsigned i=0;i<WRAM_SIZE;i++)if(refw.bytes[i]!=natw.bytes[i]){printf("WRAM %05x ref=%02x nat=%02x\n",i,refw.bytes[i],natw.bytes[i]);break;}exit(1);}
 if(which==4 || which==6 || which==17 || which==21 || which==22){
  unsigned slot=0;uint16_t pc=(uint16_t)c.pc;
  if(pc==0x99cb||pc==0x9785||pc==0x9863||pc==0x990a||pc==0x976f)slot=1;
  if(pc==0x99d8||pc==0x977b||pc==0x9860||pc==0x9904||pc==0x9761)slot=2;
  exits[which][slot]++;}
}
int main(int argc,char**argv){assert(argc==2);FILE*f=fopen(argv[1],"rb");assert(f);assert(fread(rom_data,1,sizeof rom_data,f)==sizeof rom_data);fclose(f);
 uint64_t hash=UINT64_C(1469598103934665603);for(unsigned i=0;i<sizeof rom_data;i++)hash=(hash^rom_data[i])*UINT64_C(1099511628211);
 assert(hash==UINT64_C(0x985A7978F6E47187));Rom rom={rom_data,sizeof rom_data};Cpu*cpu=cpu_init(NULL,rd,wr,idle);
 for(int section=0;section<24;section++)for(int t=0;t<1500;t++) {
  // Fill the touched low bank and retain a random high-bank sentinel.
  for(unsigned i=0;i<WRAM_SIZE;i++)refw.bytes[i]=(uint8_t)rnd();
  PortCpu c={(uint16_t)rnd(),(uint16_t)rnd(),(uint16_t)rnd(),0x1ed0,(uint16_t)((rnd()%0x17)*0x80),
   (uint8_t)((t%4==0)?0x82:(t%4==1)?0x02:(t%4==2)?0x80:0x83),(uint8_t)(rnd()&0xc7),entries[section]};
  c.a=(section==19)?(uint16_t)(rnd()%16):c.a;
  wram_w16(&refw,c.d+0x16,(uint16_t)(rnd()%9));
  wram_w16(&refw,c.d+0x1e,(uint16_t)(rnd()%4));
  if(section==22){wram_w16(&refw,c.d+0x1c,t%3==0?0:t%3==1?1:0x8000);}
  const uint16_t rec=(uint16_t)(0x185e + 0x14*(rnd()%32));wram_w16(&refw,c.d+8,rec);
  compare(cpu,c,&rom,section,t);
 }
 for(int i=0;i<24;i++)printf("section %06x tested 1500 cases, branch paths %u %u %u\n",entries[i],exits[i][0],exits[i][1],exits[i][2]);
 for(int i=0;i<24;i++)if(i==4||i==6||i==17||i==21||i==22)assert(exits[i][1]&&exits[i][2]);
 // Predicate rejects unsafe widths, bank, stack, or direct page without writes.
 Wram orig=refw;PortCpu bad={0};bad.pc=entries[0];bad.p=PORT_P_M;bad.db=0x82;bad.s=0x1ed0;assert(!native_r53_supported(&refw,&rom,&bad));
 bad.p=0;bad.db=0x7e;assert(!native_r53_supported(&refw,&rom,&bad));
 bad.db=0x82;bad.d=0x1f10;assert(!native_r53_supported(&refw,&rom,&bad));
 assert(!memcmp(&orig,&refw,sizeof orig));
 cpu_free(cpu);puts("PASS 36000 R53 native/ROM comparisons: flags, registers, 128KiB WRAM, exits and stack");}
