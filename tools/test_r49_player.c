// Differential checks against the bundled LakeSnes 65816, using the user's ROM.
// Compile with player_resume.c, cpu.c and statehandler.c; run with ROM path.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "port/player_resume.h"
#include "cpu.h"
static unsigned char rom[1048576];
static Wram reference, native;
static int cycles, fast;
static unsigned seed=0x49;
static unsigned rnd(void) { seed=seed*1664525u+1013904223u; return seed; }
static uint8_t rd(void* unused,uint32_t a) {
  (void)unused;
  if ((a&0xffff)>=0x8000) {
    cycles+=fast ? 6:8;
    return rom[((a>>16)&127)*0x8000+(a&0x7fff)];
  }
  cycles+=8; return wram_r8(&reference,a&0xffff);
}
static void wr(void* unused,uint32_t a,uint8_t v) {
  (void)unused; cycles+=8; wram_w8(&reference,a&0xffff,v);
}
static void idle(void* unused,bool wait) { (void)unused;(void)wait;cycles+=6; }
static unsigned status(Cpu* c) {
  return c->n*128+c->v*64+c->mf*32+c->xf*16+c->d*8+c->i*4+c->z*2+c->c;
}
int main(int argc,char** argv) {
  if(argc!=2)return 2;
  FILE* f=fopen(argv[1],"rb"); if(!f)return 2;
  if(fread(rom,1,sizeof(rom),f)!=sizeof(rom))return 2; fclose(f);
  uint64_t hash=UINT64_C(1469598103934665603);
  for(unsigned n=0;n<sizeof(rom);++n) { hash^=rom[n];hash*=UINT64_C(1099511628211); }
  if(hash!=UINT64_C(0x985A7978F6E47187)) { puts("Wrong reference ROM");return 2; }
  const unsigned entries[]={0x80d53d,0x80d6a8,0x80d6b8,0x80d6cb,0x80d72a};
  unsigned checked=0, exits[5][8]={{0}};
  Cpu* cpu=cpu_init(NULL,rd,wr,idle);
  for(int routine=0;routine<5;++routine)for(fast=0;fast<2;++fast)
  for(int unaligned=0;unaligned<2;++unaligned)for(int n=0;n<1000;++n) {
    memset(&reference,0x5a,sizeof(reference));
    PortCpu c={0}; c.a=(uint16_t)rnd();c.x=(uint16_t)rnd();c.y=(uint16_t)rnd();
    c.d=(uint16_t)(0x100+(n%28)*0x100+unaligned);c.s=0x1eff;c.db=0x80;
    c.p=(uint8_t)(rnd()&0xc7);c.pc=entries[routine];
    for(unsigned a=0;a<128;a+=2)wram_w16(&reference,c.d+a,(uint16_t)rnd());
    // Explicit branch coverage, including noncanonical high-bit operands.
    wram_w16(&reference,c.d+0x1a,(uint16_t)n);
    wram_w16(&reference,c.d+0x1c,(uint16_t)(n+(n%7==0)));
    wram_w16(&reference,c.d+0x4c,n%3==0?(uint16_t)rnd():0);
    wram_w16(&reference,c.d+0x1e,n%4==0?(uint16_t)rnd():0);
    wram_w16(&reference,c.d+0x16,n%5==0?(uint16_t)rnd():0);
    wram_w16(&reference,c.d+0x18,(uint16_t)(n%4));
    wram_w16(&reference,c.d+0x20,n%2?(uint16_t)rnd():0);
    wram_w16(&reference,c.d+0x6c,n%2?(uint16_t)rnd():0);
    wram_w16(&reference,c.d+0x54,(uint16_t)((n%4)*0x4000));
    native=reference;
    cpu_reset(cpu,true);cpu->resetWanted=false;cpu->e=false;
    cpu->a=c.a;cpu->x=c.x;cpu->y=c.y;cpu->dp=c.d;cpu->sp=c.s;
    cpu->db=c.db;cpu->k=0x80;cpu->pc=(uint16_t)c.pc;
    cpu->n=(c.p&128)!=0;cpu->v=(c.p&64)!=0;cpu->i=(c.p&4)!=0;
    cpu->z=(c.p&2)!=0;cpu->c=(c.p&1)!=0;cpu->mf=cpu->xf=cpu->d=false;
    PlayerResumeWork work={0};
    switch(routine) {
      case 0:player_idle_resume(&native,&c,&work);break;
      case 1:player_walk_resume(&native,&c,&work);break;
      case 2:player_walk_fire_resume(&native,&c,&work);break;
      case 3:player_walk_after_fire(&native,&c,&work);break;
      case 4:player_walk_animation(&native,&c,&work);break;
    }
    cycles=0;int ops=0;
    while((((unsigned)cpu->k<<16)|cpu->pc)!=c.pc && ops++<40)cpu_runOpcode(cpu);
    int expected=work.fast_cycles+(fast?0:2*work.rom_bytes)+(unaligned?6*work.dp_accesses:0);
    if(ops>=40 || cpu->a!=c.a || cpu->x!=c.x || cpu->y!=c.y ||
       cpu->sp!=c.s || cpu->dp!=c.d || cpu->db!=c.db || status(cpu)!=c.p ||
       memcmp(&reference,&native,sizeof(native)) || cycles!=expected) {
      printf("FAIL entry=%06X case=%d fast=%d unaligned=%d pc=%04X/%06X a=%04X/%04X p=%02X/%02X cycles=%d/%d\n",
        entries[routine],n,fast,unaligned,cpu->pc,c.pc,cpu->a,c.a,status(cpu),c.p,cycles,expected);return 1;
    }
    for(int e=0;e<8;++e)if(exits[routine][e]==c.pc)break;
      else if(!exits[routine][e]){exits[routine][e]=c.pc;break;}
    ++checked;
  }
  cpu_free(cpu);
  for(int r=0;r<5;++r) {printf("%06X exits:",entries[r]);for(int e=0;e<8&&exits[r][e];++e)printf(" %06X",exits[r][e]);puts("");}
  printf("PASS %u native/reference register, flags, full WRAM, stack and cycle comparisons\n",checked);
  return 0;
}

