// R64 actor record ROM oracle: real 65816 instructions vs native C, full WRAM/register diff.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "port/hotpaths.h"
#include "cpu.h"
static uint8_t rom_data[1048576];
static Wram refw,natw;
static unsigned seed=0x64a064u;
static unsigned rnd(void){seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;return seed;}
static Cpu *current;static int section,trial;
static uint8_t rd(void *unused,uint32_t a){(void)unused;unsigned bank=a>>16,off=a&65535;
 if(bank==0x7e||bank==0x7f)return wram_r8(&refw,a-0x7e0000);
 if(!(bank&0x40)&&off<0x2000)return wram_r8(&refw,off);
 if(off<0x8000){fprintf(stderr,"invalid hardware read %06x sec=%d trial=%d pc=%02x:%04x\n",a,section,trial,current?current->k:0,current?current->pc:0);exit(6);}
 unsigned i=(bank&127)*0x8000+(off&0x7fff);assert(i<sizeof rom_data);return rom_data[i];}
static void wr(void *unused,uint32_t a,uint8_t v){(void)unused;unsigned b=a>>16,o=a&65535;
 if(b==0x7e||b==0x7f)wram_w8(&refw,a-0x7e0000,v);
 else {if((b&0x40)||o>=0x2000){fprintf(stderr,"invalid hardware write %06x sec=%d trial=%d\n",a,section,trial);exit(7);}wram_w8(&refw,o,v);}}
static void idle(void*u,bool w){(void)u;(void)w;}
static uint8_t status(const Cpu*c){return (c->n?128:0)|(c->v?64:0)|(c->mf?32:0)|(c->xf?16:0)|(c->d?8:0)|(c->i?4:0)|(c->z?2:0)|(c->c?1:0);}
static void load(Cpu*cpu,const PortCpu*c){cpu_reset(cpu,true);cpu->resetWanted=false;cpu->e=false;
 cpu->a=c->a;cpu->x=c->x;cpu->y=c->y;cpu->dp=c->d;cpu->sp=c->s;cpu->db=c->db;
 cpu->k=(uint8_t)(c->pc>>16);cpu->pc=(uint16_t)c->pc;
 cpu->n=!!(c->p&128);cpu->v=!!(c->p&64);cpu->i=!!(c->p&4);
 cpu->z=!!(c->p&2);cpu->c=!!(c->p&1);cpu->mf=cpu->xf=cpu->d=false;}
static const uint32_t entries[]={0x81f204,0x81f206,0x81f208,0x81f20b,0x81f20c,0x81f20e,0x81f211,0x81f214,0x81f217,0x81f219,0x81f21c,0x81f21f,0x81f222,0x81f225,0x81f228,0x81f22b,0x81f22e,0x81f231,0x81f234,0x81f237,0x81f23a,0x81f23d,0x81f23f,0x81f241,0x81f244,0x81f246,0x81f249};
static const uint32_t exits[][3]={{0x81f24b},{0x81f24b},{0x81f24b},{0x81f24b},{0x81f24b},{0x81f24b},{0x81f24b},{0x81f24b},{0x81f24b},{0x81f24b},{0x81f24b},{0x81f24b},{0x81f24b},{0x81f24b},{0x81f24b},{0x81f24b},{0x81f24b},{0x81f24b},{0x81f24b},{0x81f24b},{0x81f24b},{0x81f24b},{0x81f24b},{0x81f24b},{0x81f24b},{0x81f24b},{0x81f24b}};
static int hits[27][3];
int main(int argc,char**argv){if(argc!=2)return 2;FILE*f=fopen(argv[1],"rb");assert(f);
 assert(fread(rom_data,1,sizeof rom_data,f)==sizeof rom_data);fclose(f);
 uint64_t hash=UINT64_C(1469598103934665603);
 for(unsigned i=0;i<sizeof rom_data;i++) hash=(hash^rom_data[i])*UINT64_C(1099511628211);
 assert(hash==UINT64_C(0x985A7978F6E47187));Rom rom={rom_data,sizeof rom_data};Cpu*cpu=cpu_init(NULL,rd,wr,idle);
 for(section=0;section<27;section++)for(trial=0;trial<600;trial++){
  for(unsigned i=0;i<WRAM_SIZE;i++) refw.bytes[i]=(uint8_t)rnd();
  PortCpu c={(uint16_t)rnd(),(uint16_t)(rnd()%0x600), (uint16_t)(rnd()%0x600),
      (uint16_t)(0x150+(rnd()%0x400)), (uint16_t)(0x100+(rnd()%0x10)*0x60),
      (uint8_t)(trial%2?0x80:0x02),(uint8_t)(rnd()&0xc7),entries[section]};
  c.db=(trial%2)?0x80:0x00;
  c.d=(uint16_t)(0x100+(rnd()%0x10)*0x40);
  c.s=(uint16_t)(0x300+(rnd()%0x100));
  c.y=(uint16_t)(0x200+(rnd()%0x900));
  // Force positive, negative and overflow-relevant clock values.
  wram_w16(&refw,0xde,trial%4==0?0:trial%4==1?0x7fff:trial%4==2?0x8000:0xffff);
  natw=refw;
  if(!native_r64_supported(&natw,&rom,&c)){fprintf(stderr,"REJECT section=%d pc=%06x t=%d\n",section,c.pc,trial);return 4;}
  PortCpu original=c;
  native_r64_actor_record(&natw,&rom,&c,NULL);
  load(cpu,&original);current=cpu;
  int steps=0;while((((uint32_t)cpu->k<<16)|cpu->pc)!=c.pc && steps++ < 80)cpu_runOpcode(cpu);
  int slot= -1;for(int i=0;i<3;i++)if(exits[section][i]&&exits[section][i]==c.pc)slot=i;
  if(slot<0||steps>=80||(((uint32_t)cpu->k<<16)|cpu->pc)!=c.pc || cpu->a!=c.a || cpu->x!=c.x || cpu->y!=c.y || cpu->sp!=c.s || cpu->dp!=c.d || cpu->db!=c.db ||status(cpu)!=c.p||memcmp(&refw,&natw,sizeof(refw))){
   fprintf(stderr,"FAIL section=%d trial=%d from=%06x exit=%06x/%02x:%04x steps=%d A=%04x/%04x X=%04x/%04x Y=%04x/%04x P=%02x/%02x S=%04x/%04x\n",section,trial,original.pc,c.pc,cpu->k,cpu->pc,steps,c.a,cpu->a,c.x,cpu->x,c.y,cpu->y,c.p,status(cpu),c.s,cpu->sp);
   for(unsigned i=0;i<WRAM_SIZE;i++)if(refw.bytes[i]!=natw.bytes[i]){fprintf(stderr,"WRAM %05x ref=%02x nat=%02x\n",i,refw.bytes[i],natw.bytes[i]);break;}return 5;
  }
  hits[section][slot]++;
 }
 for(int i=0;i<27;i++){printf("section %06x ROM/native PASS 600 exits %d/%d/%d\n",entries[i],hits[i][0],hits[i][1],hits[i][2]);assert(hits[i][0]==600);}
 cpu_free(cpu);puts("PASS 16200 R64 full-state 65816-ROM/native comparisons, registers, flags, stack, PC, 128KiB WRAM");
 return 0;}
