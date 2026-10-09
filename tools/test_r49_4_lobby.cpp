// Production menu renderer compiled with only the Xbox presentation services mocked.
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
typedef uint32_t DWORD;
#define ZAMN_FB_W 512
#define ZAMN_FB_H 480
#define __attribute__(x)
static DWORD tick=1000,slept=0;
static bool locked=false;
static uint8_t texture[512*480*4],background_pixels[256*224*4];
static DWORD GetTickCount(){return tick;}
static void Sleep(DWORD n){slept+=n;tick+=n;}
static void Xbox_Log(const char*,...){}
static void background(uint8_t*p,int pitch){assert(!locked);for(int y=0;y<224;++y)memcpy(p+y*pitch,background_pixels+y*1024,1024);tick+=8;}
static void (*s_background)(uint8_t*,int)=background;
static void Xbox_D3D_BeginDraw(uint8_t**p,int*pitch){assert(!locked);locked=true;*p=texture;*pitch=2048;}
static void Xbox_D3D_SetSourceRect(int x,int y,int w,int h){assert(x==0&&y==0&&w==256&&h==224);}
static void Xbox_D3D_PrepareFrame(){assert(locked);locked=false;}
static void Xbox_D3D_Present(){assert(!locked);tick+=8;}
#include "r494_menu.inc"
static void picture(const char*name){
  FILE*f=fopen(name,"wb");assert(f);uint8_t h[54]={66,77};uint32_t n=54+512*448*3,o=54,z=40,w=512,y=(uint32_t)-448;
  memcpy(h+2,&n,4);memcpy(h+10,&o,4);memcpy(h+14,&z,4);memcpy(h+18,&w,4);memcpy(h+22,&y,4);h[26]=1;h[28]=24;fwrite(h,1,54,f);
  for(int yy=0;yy<448;++yy)for(int x=0;x<512;++x)fwrite(texture+(yy/2)*2048+(x/2)*4,1,3,f);fclose(f);
}
int main(int argc,char**argv){
  assert(argc==2);FILE*f=fopen(argv[1],"rb");assert(f);char line[100];for(int i=0;i<3;++i)fgets(line,100,f);
  static uint8_t rgb[512*480*3];assert(fread(rgb,1,sizeof(rgb),f)==sizeof(rgb));fclose(f);
  for(int y=0;y<224;++y)for(int x=0;x<256;++x){uint8_t*p=background_pixels+(y*256+x)*4,*q=rgb+((y*2+16)*512+x*2)*3;p[0]=q[2];p[1]=q[1];p[2]=q[0];}
  menu_frame("PUBLIC ROOMS","> HOST PUBLIC ROOM","  JOIN PUBLIC ROOM","  BACK","AUTO NAT + RELAY FALLBACK","A SELECT   B BACK");menu_yield();assert(slept==0);picture("public-rooms-preview.bmp");
  menu_frame("HOST DIRECT","PUBLIC IP 203.0.113.10","LOCAL IP 192.168.1.196","PLAYER 2 READY - A OR START","DELAY MANUAL 3F  ROLLBACK: ENABLED","LEFT/RIGHT DELAY  RS ROLLBACK  A START  B BACK");picture("host-direct-preview.bmp");
  menu_ip_editor_frame("JOIN DIRECT","192.168.001.100",4,"ENTER HOST IPV4","LOCAL IP 192.168.1.217","UDP 6464   A CONNECT   B BACK");picture("join-direct-preview.bmp");
  // Panel really blends live pixels; untouched border area retains the scene.
  uint32_t before=((uint32_t*)background_pixels)[170*256+120];
  assert(((uint32_t*)(texture+170*2048))[120]==((before>>2)&0x003f3f3fu));
  assert(((uint32_t*)texture)[3]==((uint32_t*)background_pixels)[3]);
  puts("PASS native256 cached composition, GPU lock order, translucent panel, no additive wait, all lobby previews");
}
