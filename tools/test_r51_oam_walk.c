#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "port/hotpaths.h"
#include "cpu.h"
static uint8_t rom_data[1048576];
static Wram refw,natw;
static unsigned seed=0x51BD30u;
static unsigned rnd(void){seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;return seed;}
static uint8_t rd(void*unused,uint32_t a){(void)unused;unsigned b=a>>16,o=a&65535;
 if(b==0x7e||b==0x7f)return wram_r8(&refw,a-0x7e0000);
 if(!(b&0x40)&&o<0x2000)return wram_r8(&refw,o);
 if(o<0x8000)return 0;unsigned i=(b&127)*0x8000+(o&0x7fff);return i<sizeof(rom_data)?rom_data[i]:0;}
static void wr(void*unused,uint32_t a,uint8_t v){(void)unused;unsigned b=a>>16,o=a&65535;
 if(b==0x7e||b==0x7f)wram_w8(&refw,a-0x7e0000,v);
 else {assert(!(b&0x40)&&o<0x2000);wram_w8(&refw,o,v);} }
static void idle(void*u,bool w){(void)u;(void)w;}
static uint8_t status(const Cpu*c){return c->n*128+c->v*64+c->mf*32+c->xf*16+c->d*8+c->i*4+c->z*2+c->c;}
static void cpu_load(Cpu*cpu,const PortCpu*c){cpu_reset(cpu,true);cpu->resetWanted=false;cpu->e=false;cpu->a=c->a;cpu->x=c->x;cpu->y=c->y;cpu->dp=c->d;cpu->sp=c->s;cpu->db=c->db;cpu->k=0x80;cpu->pc=(uint16_t)c->pc;cpu->n=!!(c->p&128);cpu->v=!!(c->p&64);cpu->i=!!(c->p&4);cpu->z=!!(c->p&2);cpu->c=!!(c->p&1);cpu->mf=cpu->xf=cpu->d=false;}
static void compare(Cpu*cpu,PortCpu in,Rom*rom,int trial){PortCpu c=in;natw=refw;assert(native_oam_walk_supported(&natw,rom,&c));native_oam_walk(&natw,rom,&c,NULL);cpu_load(cpu,&in);unsigned steps=0;uint16_t e1=0,e2=0;
 if(in.pc==0x80bd30u||in.pc==0x80bdb7u){e1=0xbdb4;e2=0xbdcc;} else {e1=e2=0xbde2;}
 while(cpu->pc!=e1&&cpu->pc!=e2&&steps++<2000)cpu_runOpcode(cpu);
 if(steps>=2000||(((unsigned)cpu->k<<16)|cpu->pc)!=c.pc||cpu->a!=c.a||cpu->x!=c.x||cpu->y!=c.y||cpu->dp!=c.d||cpu->sp!=c.s||cpu->db!=c.db||status(cpu)!=c.p||memcmp(&refw,&natw,sizeof(refw))){
  printf("FAIL trial=%d entry=%06x pc=%04x/%06x A=%04x/%04x X=%04x/%04x Y=%04x/%04x D=%04x/%04x DB=%02x/%02x S=%04x/%04x P=%02x/%02x steps=%u\n",trial,(unsigned)in.pc,cpu->pc,(unsigned)c.pc,cpu->a,c.a,cpu->x,c.x,cpu->y,c.y,cpu->dp,c.d,cpu->db,c.db,cpu->sp,c.s,status(cpu),c.p,steps);
  for(unsigned i=0;i<sizeof(refw);i++)if(refw.bytes[i]!=natw.bytes[i]){printf("WRAM %05x %02x/%02x\n",i,refw.bytes[i],natw.bytes[i]);break;}exit(1);} }
static void build_list(int trial){for(unsigned i=0;i<WRAM_SIZE;i++)refw.bytes[i]=(uint8_t)rnd();unsigned cnt=(unsigned)(trial%33);wram_w16(&refw,0x9c,(uint16_t)(cnt*2));
 for(unsigned i=0;i<cnt;i++){uint16_t rec=(uint16_t)(0x185e + i*0x14);wram_w16(&refw,0x137e + i*2,rec);uint16_t flags=(uint16_t)rnd();if((trial+i)%4)flags|=0x8000;else flags&=0x7fff;wram_w16(&refw,rec,flags);wram_w16(&refw,rec+2,(uint16_t)rnd());wram_w16(&refw,rec+4,(uint16_t)(rnd()%64));wram_w16(&refw,rec+6,(uint16_t)rnd());wram_w16(&refw,rec+0x10,(uint16_t)rnd());
  uint16_t ptr=(uint16_t)(0x9000+((i*32+trial*8)&0x1ff8));uint16_t bank=(uint16_t)(((i+trial)&1)?0x8f:0x90);wram_w16(&refw,rec+8,ptr);wram_w16(&refw,rec+0x0a,bank);unsigned off=(bank&127)*0x8000+(ptr&0x7fff);rom_data[off]=(uint8_t)(((trial+i)%5)?1:0);rom_data[off+1]=(uint8_t)rnd(); }
}
int main(int argc,char**argv){assert(argc==2);FILE*f=fopen(argv[1],"rb");assert(f);assert(fread(rom_data,1,sizeof rom_data,f)==sizeof rom_data);fclose(f);Rom rom={rom_data,sizeof rom_data};Cpu*cpu=cpu_init(NULL,rd,wr,idle);unsigned exits[3][2]={{0}};
 for(int t=0;t<12000;t++){
  int kind=t%3;PortCpu c={(uint16_t)rnd(),(uint16_t)rnd(),(uint16_t)rnd(),0x1ef0,0,0x80,(uint8_t)(rnd()&0xc7),kind==0?0x80bd30u:kind==1?0x80bdb7u:0x80bdd0u};
  if(kind<2){build_list(t);wram_w16(&refw,0x88,0);uint16_t n=wram_r16(&refw,0x9c);if(kind==1){if(n<2){wram_w16(&refw,0x9c,2);uint16_t rec=0x185e;wram_w16(&refw,0x137e,rec);wram_w16(&refw,rec,0);n=2;}wram_w16(&refw,0x9a,(uint16_t)(2*(rnd()%(n/2))));c.x=(t%7==0)?0x200:(uint16_t)(4*(rnd()%128));wram_w16(&refw,0x88,c.x);}
  } else {for(unsigned i=0;i<WRAM_SIZE;i++)refw.bytes[i]=(uint8_t)rnd();uint16_t od=(uint16_t)((rnd()%0x1c00)&0xfff0);uint8_t odb=(t&1)?(uint8_t)(rnd()%0x20):(uint8_t)(0x80+(rnd()%0x20));wram_w16(&refw,(uint16_t)(od+0x20),(uint16_t)rnd());wram_w8(&refw,c.s+1,(uint8_t)od);wram_w8(&refw,c.s+2,(uint8_t)(od>>8));wram_w8(&refw,c.s+3,odb);}
  PortCpu n=c;Wram before=refw;if(!native_oam_walk_supported(&before,&rom,&n)){printf("SUPPORT FAIL t=%d kind=%d X=%04x oi=%04x n=%04x cur=%04x S=%04x P=%02x\n",t,kind,c.x,wram_r16(&before,0x88),wram_r16(&before,0x9c),wram_r16(&before,0x9a),c.s,c.p);printf(" savedD=%04x savedDB=%02x dp20=%04x\n",(unsigned)(wram_r8(&before,c.s+1)|((unsigned)wram_r8(&before,c.s+2)<<8)),wram_r8(&before,c.s+3),(unsigned)((wram_r8(&before,c.s+1)|((unsigned)wram_r8(&before,c.s+2)<<8))+0x20));return 2;}native_oam_walk(&before,&rom,&n,NULL);exits[kind][((uint16_t)n.pc)==(kind==2?0xbde2:0xbdcc)]++;compare(cpu,c,&rom,t);
 }
 printf("BD30 exits emitter=%u overlap=%u\n",exits[0][0],exits[0][1]);printf("BDB7 exits emitter=%u overlap=%u\n",exits[1][0],exits[1][1]);printf("BDD0 exits=%u\n",exits[2][1]);assert(exits[0][0]&&exits[0][1]&&exits[1][0]&&exits[1][1]&&exits[2][1]);puts("PASS 12000 R51 OAM-walker native/ROM comparisons: registers, flags, full WRAM, stack and exits");cpu_free(cpu);}
