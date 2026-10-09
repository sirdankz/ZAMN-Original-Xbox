// Self-contained owned-ROM pattern verification; test pass ROM path as argv[1].
// The ROM is NOT packaged in the public source.
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <assert.h>
#include <string.h>
static long lorom(unsigned bank,unsigned addr){return (long)(bank&0x7f)*0x8000 +(addr&0x7fff);}
int main(int argc,char**argv) {
 assert(argc==2);FILE*f=fopen(argv[1],"rb");assert(f);
 static uint8_t rom[1048576];assert(fread(rom,1,sizeof rom,f)==sizeof rom);fclose(f);
 static const uint8_t pause[]={
  0xad,0x6e,0x00,0x0d,0x70,0x00,0x89,0x00,0x10,0xd0,0xf5,
  0xad,0x6e,0x00,0x0d,0x70,0x00,0x89,0x00,0x10,0xf0,0xf5,
  0xad,0x6e,0x00,0x0d,0x70,0x00,0x89,0x00,0x10,0xd0,0xf5};
 assert(sizeof pause==33);
 assert(memcmp(rom+lorom(0x80,0x89c8),pause,sizeof pause)==0);
 static const uint8_t safe[]={0xc9,0x5c,0x00,0xb0,0x02,0x18,0x6b};
 assert(memcmp(rom+lorom(0x81,0xd7f6),safe,sizeof safe)==0);
 assert(memcmp(rom+lorom(0x82,0x9a6d),safe,sizeof safe)==0);
 assert(memcmp(rom+lorom(0x81,0xd7f6),rom+lorom(0x82,0x9a6d),64)==0);
 puts("R70 ROM PASS: pause release/press/release loop and $82:9A6D ignore path match owned ROM");
 return 0;
}
