// Differential execution of the actual USA ROM against the native sections.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "port/hotpaths.h"
#include "cpu.h"
static uint8_t rom_data[1048576];
static Wram ref, nat;
static int cycles, fast;
static unsigned seed=0x50668;
static unsigned rnd(void) { seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;return seed; }
static uint8_t rd(void* unused,uint32_t a) {
  (void)unused;unsigned bank=a>>16, off=a&65535;
  if(bank==0x7e || bank==0x7f) { cycles+=8;return wram_r8(&ref,a-0x7e0000); }
  if(!(bank&0x40) && off<0x2000) { cycles+=8;return wram_r8(&ref,off); }
  assert(off>=0x8000);unsigned i=(bank&127)*0x8000+(off&0x7fff);assert(i<sizeof(rom_data));
  cycles+=fast && bank>=0x80?6:8;return rom_data[i];
}
static void wr(void* unused,uint32_t a,uint8_t v) {
  (void)unused;unsigned bank=a>>16,off=a&65535;cycles+=8;
  if(bank==0x7e || bank==0x7f) wram_w8(&ref,a-0x7e0000,v);
  else { assert(!(bank&0x40) && off<0x2000);wram_w8(&ref,off,v); }
}
static void idle(void* unused,bool wait) { (void)unused;(void)wait;cycles+=6; }
static uint8_t status(const Cpu* c) { return c->n*128+c->v*64+c->mf*32+c->xf*16+c->d*8+c->i*4+c->z*2+c->c; }
static unsigned exits[10][2];
static void compare(Cpu* cpu,PortCpu initial,Rom* rom,int which,int trial) {
  PortCpu c=initial;nat=ref;NativeHotWork k={0};
  bool ok=which<2?native_overlap_supported(&nat,&c):native_sprite_supported(&nat,rom,&c);
  assert(ok);
  if(which<2)native_overlap_scan(&nat,&c,&k);else native_sprite_emit(&nat,rom,&c,&k);
  cpu_reset(cpu,true);cpu->resetWanted=false;cpu->e=false;
  cpu->a=initial.a;cpu->x=initial.x;cpu->y=initial.y;cpu->dp=initial.d;cpu->sp=initial.s;
  cpu->db=initial.db;cpu->k=0x80;cpu->pc=(uint16_t)initial.pc;
  cpu->n=!!(initial.p&128);cpu->v=!!(initial.p&64);cpu->i=!!(initial.p&4);
  cpu->z=!!(initial.p&2);cpu->c=!!(initial.p&1);cpu->mf=cpu->xf=cpu->d=false;
  cycles=0;unsigned steps=0;
  // Stop on the section's external-call boundary or final return opcode.
  uint16_t call_pc=0, ret_pc=0;
  if(which<2) { call_pc=0xbf0d; ret_pc=0xbf1a; }
  else {
    static const uint16_t calls[4]={0xba98,0xbb08,0xbb7e,0xbbfb};
    static const uint16_t rets[4]={0xbab9,0xbb2f,0xbba5,0xbc22};
    unsigned variant=(unsigned)(which-2)/2;
    call_pc=calls[variant]; ret_pc=rets[variant];
  }
  while(cpu->pc!=call_pc && cpu->pc!=ret_pc && steps++<40000)cpu_runOpcode(cpu);
  int expected=k.fast_cycles+(fast?0:2*k.rom_bytes);
  if(steps>=40000 || (((unsigned)cpu->k<<16)|cpu->pc)!=c.pc || cpu->a!=c.a || cpu->x!=c.x || cpu->y!=c.y ||
     cpu->dp!=c.d || cpu->sp!=c.s || cpu->db!=c.db || status(cpu)!=c.p || memcmp(&ref,&nat,sizeof(ref)) || cycles!=expected) {
    printf("FAIL section=%d trial=%d fast=%d PC=%04x/%06x A=%04x/%04x X=%04x/%04x Y=%04x/%04x P=%02x/%02x cycles=%d/%d steps=%u\n",
      which,trial,fast,cpu->pc,c.pc,cpu->a,c.a,cpu->x,c.x,cpu->y,c.y,status(cpu),c.p,cycles,expected,steps);
    for(unsigned i=0;i<sizeof(ref);++i)if(ref.bytes[i]!=nat.bytes[i]){printf("WRAM %05x %02x/%02x\n",i,ref.bytes[i],nat.bytes[i]);break;}
    exit(1);
  }
  exits[which][((uint16_t)c.pc)==ret_pc]++;
}
int main(int argc,char**argv) {
  assert(argc==2);FILE*f=fopen(argv[1],"rb");assert(f);assert(fread(rom_data,1,sizeof(rom_data),f)==sizeof(rom_data));fclose(f);
  uint64_t hash=UINT64_C(1469598103934665603);
  for(unsigned i=0;i<sizeof(rom_data);++i)hash=(hash^rom_data[i])*UINT64_C(1099511628211);
  assert(hash==UINT64_C(0x985A7978F6E47187));
  Rom rom={rom_data,sizeof(rom_data)};Cpu*cpu=cpu_init(NULL,rd,wr,idle);
  const unsigned entries[]={0x80bec9,0x80bf12,
    0x80ba51,0x80ba9b, 0x80baba,0x80bb0b, 0x80bb30,0x80bb81, 0x80bba6,0x80bbfe};
  const uint16_t edges[]={0,7,8,15,16,0xfff0,0xfff1,0xffff,0xdf,0xe0,0xff,0x100,0x7fff,0x8000};
  for(int section=0;section<10;++section)for(fast=0;fast<2;++fast)for(int n=0;n<1500;++n) {
    for(unsigned i=0;i<WRAM_SIZE;++i)ref.bytes[i]=(uint8_t)rnd();
    PortCpu c={(uint16_t)rnd(),(uint16_t)rnd(),(uint16_t)rnd(),0x1eff,0,0x80,(uint8_t)(rnd()&0xc7),entries[section]};
    if(section<2) {
      unsigned count=n%33;wram_w16(&ref,0x9c,(uint16_t)(count*2));
      for(unsigned i=0;i<32;++i) {
        unsigned rec=0x185e + i*0x14;wram_w16(&ref,0x137e + i*2,(uint16_t)rec);
        wram_w16(&ref,rec+0x0e,n%5==0?0:n%7==0?1:(uint16_t)(rnd()%10));
        wram_w16(&ref,rec+2,n%3?(uint16_t)(rnd()%32):edges[rnd()%14]);
        wram_w16(&ref,rec+6,n%3?(uint16_t)(rnd()%32):edges[rnd()%14]);
      }
      if(section==1) {
        c.y=(uint16_t)(2*(rnd()%32));wram_w16(&ref,0x3c,(uint16_t)(2*(rnd()%32)));
        wram_w16(&ref,0x38,edges[rnd()%14]);wram_w16(&ref,0x3a,edges[rnd()%14]);wram_w16(&ref,0x4a,(uint16_t)(rnd()%10));
      }
    } else {
      unsigned count=1+rnd()%128;uint16_t ptr=(uint16_t)(0x8000+(rnd()%256)*8);
      unsigned bank=n%2?0x8f:0x90;unsigned off=(bank&127)*0x8000+(ptr&0x7fff);
      wram_w16(&ref,0x86,(uint16_t)count);wram_w16(&ref,0x8a,ptr);wram_w8(&ref,0x8c,(uint8_t)bank);
      c.x=(uint16_t)(4*(rnd()%128));wram_w16(&ref,0x88,c.x);
      uint16_t dx=edges[n%14],dy=edges[(n/14)%14];
      wram_w16(&ref,0x8e,0);wram_w16(&ref,0x90,0);
      for(unsigned i=0;i<count;++i) {
        uint16_t x=i?edges[rnd()%14]:dx,y=i?edges[rnd()%14]:dy;
        rom_data[off+i*8]=(uint8_t)x;rom_data[off+i*8+1]=(uint8_t)(x>>8);
        rom_data[off+i*8+2]=(uint8_t)y;rom_data[off+i*8+3]=(uint8_t)(y>>8);
      }
    }
    compare(cpu,c,&rom,section,n);
  }
  for(int i=0;i<10;++i) {printf("section %06X exits call=%u return=%u\n",entries[i],exits[i][0],exits[i][1]);assert(exits[i][0]&&exits[i][1]);}
  // Guard failures must not mutate input, and keep unusual/hardware states on fallback.
  memset(&ref,0,sizeof(ref));PortCpu c={0,0,0,0x1eff,0,0x80,0,0x80bec9};
  wram_w16(&ref,0x9c,3);assert(!native_overlap_supported(&ref,&c));
  wram_w16(&ref,0x9c,66);assert(!native_overlap_supported(&ref,&c));
  wram_w16(&ref,0x9c,4);wram_w16(&ref,0x137e,0x2100);assert(!native_overlap_supported(&ref,&c));
  wram_w16(&ref,0x9c,0);c.p=0x20;assert(!native_overlap_supported(&ref,&c));c.p=0;c.d=1;assert(!native_overlap_supported(&ref,&c));
  c.d=0;c.db=0x7e;assert(!native_overlap_supported(&ref,&c));
  c.db=0x80;c.pc=0x80ba51;assert(!native_sprite_supported(&ref,&rom,&c));
  cpu_free(cpu);puts("PASS 30000 native/ROM comparisons: registers, flags, full WRAM, stack, exits and exact fast/slow cycles; all four sprite flip variants + resumes; both exits covered per section; invalid-state guards");
}
