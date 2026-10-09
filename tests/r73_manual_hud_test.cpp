#include "platform/xbox/xbox_platform.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <cstdarg>
static FILE* R73_TestFopen(const char* name,const char* mode){
  const char* leaf=strstr(name,"spread");
  return leaf?fopen((std::string("manual/")+leaf).c_str(),mode):nullptr;
}
#define fopen R73_TestFopen
#include "platform/xbox/xbox_d3d.cpp"
#undef fopen
extern "C" void Xbox_Log(const char*,...){}
extern "C" void Xbox_Netplay_DrawText(uint8_t*,int,int,int,const char*,int,uint32_t){}
static unsigned ink_count(FakeTexture* tex,int top,int bottom){
  unsigned count=0;
  const uint32_t* px=(const uint32_t*)tex->bytes.data();
  for(int y=top;y<bottom;++y)for(int x=0;x<1024;++x)
    if(px[y*1024+x]!=0xFF141414u)++count;
  return count;
}
int main(){
  FakeDevice dev;s_dev=&dev;
  assert(Xbox_D3D_ManualOpen(0));
  // R72 clipped all hints at y>=480. R73 must write actual opaque HUD pixels
  // at 760..832 while leaving the high-res book region untouched.
  assert(ink_count(s_manual_tex,760,762)==2048);
  assert(ink_count(s_manual_tex,770,794)>350);
  assert(ink_count(s_manual_tex,804,830)>1000);
  const uint32_t* px=(const uint32_t*)s_manual_tex->bytes.data();
  assert(px[770*1024+23]!=0xFF141414u || ink_count(s_manual_tex,770,792)>0);
  const unsigned original=ink_count(s_manual_tex,760,832);
  assert(Xbox_D3D_ManualPage(9));
  assert(ink_count(s_manual_tex,760,832)>2000);
  assert(ink_count(s_manual_tex,760,832)!=0);
  Xbox_D3D_ManualZoom(1);Xbox_D3D_ManualPan(128,128);
  assert(s_manual_zoom_level==1);
  // Panning and zooming modify paper UVs, not the texture's fixed footer.
  assert(ink_count(s_manual_tex,760,832)>2000);
  assert(original>2000);
  Xbox_D3D_ManualClose();assert(!s_manual_tex&&!s_manual_view);
  std::puts("R73 manual fixed HUD pixels, separator, page turns, zoom/pan and cleanup PASS");
}
