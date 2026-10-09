// R60 ROM oracle: real 65816 instructions vs native C, full WRAM/register diff.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "port/hotpaths.h"
#include "cpu.h"
static uint8_t rom_data[1048576];
static Wram refw,natw;
static unsigned seed=0x60c0111u;
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
static const uint32_t entries[]={0x80be91,0x80be93,0x80be95,0x80be97,0x80be99,0x80be9b,0x81f1cd,0x81f1d0,0x82993d,0x829942,0x829944,0x82994a,0x82994d,0x82994f,0x829952,0x829955,0x829958,0x82995b};
static const uint32_t exits[][3]={{0x80be9e},{0x80be9e},{0x80be9e},{0x80be9e},{0x80be9e},{0x80be9e},
 {0x81f1d3},{0x81f1d3},
 {0x829927,0x829946,0x829969}, {0x829946,0x829969},{0x829946,0x829969},
 {0x82995e},{0x82995e},{0x82995e},{0x82995e},{0x82995e},{0x82995e},{0x82995e}};
static int hits[18][3];
int main(int argc,char**argv){if(argc!=2)return 2;FILE*f=fopen(argv[1],"rb");assert(f);
 assert(fread(rom_data,1,sizeof rom_data,f)==sizeof rom_data);fclose(f);
 uint64_t hash=UINT64_C(1469598103934665603);
 for(unsigned i=0;i<sizeof rom_data;i++) hash=(hash^rom_data[i])*UINT64_C(1099511628211);
 assert(hash==UINT64_C(0x985A7978F6E47187));Rom rom={rom_data,sizeof rom_data};Cpu*cpu=cpu_init(NULL,rd,wr,idle);
 for(section=0;section<18;section++)for(trial=0;trial<900;trial++){
  for(unsigned i=0;i<WRAM_SIZE;i++) refw.bytes[i]=(uint8_t)rnd();
  PortCpu c={(uint16_t)rnd(),(uint16_t)(rnd()%0x600), (uint16_t)(rnd()%0x600),
      (uint16_t)(0x150+(rnd()%0x400)), (uint16_t)(0x100+(rnd()%0x10)*0x60),
      (uint8_t)(trial%2?0x80:0x02),(uint8_t)(rnd()&0xc7),entries[section]};
  if(section<=5) {
    wram_w16(&refw,c.d+0x3c,(uint16_t)(rnd()%64u));
    c.y=(uint16_t)(rnd()%64u);
    c.x=(uint16_t)(rnd()%((0x1fff-c.d-0x0f)+1));
  }
  if(section==6) {c.x=(uint16_t)(rnd()%0x500);c.y=(uint16_t)(rnd()%0x500);}
  if(section==7) {c.y=(uint16_t)(rnd()%0x500);}
  if(section==8) { // both outcomes, plus both score-zero and nonzero
    c.p=(uint8_t)((c.p&~PORT_P_Z) | ((trial%3==0)?PORT_P_Z:0));
    wram_w16(&refw,c.d+0x20,(trial%3==1)?0:0x1234);
  }
  if(section==9) wram_w16(&refw,c.d+0x20,(trial%2)?0:0x4321);
  if(section==10)c.p=(uint8_t)((c.p&~PORT_P_Z)|((trial%2)?0:PORT_P_Z));
  if(section>=11 && section<=12)wram_w16(&refw,c.d+0x08,(uint16_t)(rnd()%0x500));
  if(section>=13)c.y=(uint16_t)(rnd()%0x500);
  natw=refw;
  if(!native_r60_supported(&natw,&rom,&c)){fprintf(stderr,"REJECT section=%d pc=%06x t=%d\n",section,c.pc,trial);return 4;}
  PortCpu original=c;
  native_r60_connected_blocks(&natw,&rom,&c,NULL);
  load(cpu,&original);current=cpu;
  int steps=0;while((((uint32_t)cpu->k<<16)|cpu->pc)!=c.pc && steps++ < 70)cpu_runOpcode(cpu);
  int slot= -1;for(int i=0;i<3;i++)if(exits[section][i]&&exits[section][i]==c.pc)slot=i;
  if(slot<0||steps>=70||(((uint32_t)cpu->k<<16)|cpu->pc)!=c.pc || cpu->a!=c.a || cpu->x!=c.x || cpu->y!=c.y || cpu->sp!=c.s || cpu->dp!=c.d || cpu->db!=c.db ||status(cpu)!=c.p||memcmp(&refw,&natw,sizeof(refw))){
   fprintf(stderr,"FAIL section=%d trial=%d from=%06x exit=%06x/%02x:%04x steps=%d A=%04x/%04x X=%04x/%04x Y=%04x/%04x P=%02x/%02x S=%04x/%04x\n",section,trial,original.pc,c.pc,cpu->k,cpu->pc,steps,c.a,cpu->a,c.x,cpu->x,c.y,cpu->y,c.p,status(cpu),c.s,cpu->sp);
   for(unsigned i=0;i<WRAM_SIZE;i++)if(refw.bytes[i]!=natw.bytes[i]){fprintf(stderr,"WRAM %05x ref=%02x nat=%02x\n",i,refw.bytes[i],natw.bytes[i]);break;}return 5;
  }
  hits[section][slot]++;
 }
 for(int i=0;i<18;i++){printf("section %06x ROM/native PASS 900 exits %d/%d/%d\n",entries[i],hits[i][0],hits[i][1],hits[i][2]);if(i==8)assert(hits[i][0]&&hits[i][1]&&hits[i][2]); else if(exits[i][1])assert(hits[i][0]&&hits[i][1]);}
 cpu_free(cpu);puts("PASS 16200 R60 full-state 65816-ROM/native comparisons, registers, flags, stack, PC, 128KiB WRAM");
 return 0;}
