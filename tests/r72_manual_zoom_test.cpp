#include "platform/xbox/xbox_platform.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <cstdarg>
static FILE* R71_TestFopen(const char* name,const char* mode) {
  const char* leaf=strstr(name,"spread");
  if(!leaf)return nullptr;
  return fopen((std::string("manual/")+leaf).c_str(),mode);
}
#define fopen R71_TestFopen
#include "platform/xbox/xbox_d3d.cpp"
#undef fopen
extern "C" void Xbox_Log(const char*,...){}
extern "C" void Xbox_Netplay_DrawText(uint8_t*,int,int,int,const char*,int,uint32_t){}
int main(){
  FakeDevice device;
  s_dev=&device;
  assert(Xbox_D3D_ManualOpen(0));
  assert(s_manual_view && s_manual_tex);
  assert(s_manual_tex->bytes.size()==(size_t)1024*832*4);
  const auto first=s_manual_tex->bytes[100*1024*4+50*4+2];
  assert(Xbox_D3D_ManualPage(4));
  const auto second=s_manual_tex->bytes[100*1024*4+50*4+2];
  assert(first!=second);
  assert(!Xbox_D3D_ManualPage(-1));
  assert(!Xbox_D3D_ManualPage(10));
  assert(s_manual_zoom_level==0 && s_manual_cx==512 && s_manual_cy==380);
  Xbox_D3D_ManualZoom(1);
  assert(s_manual_zoom_level==1);
  Xbox_D3D_ManualPan(128,128);
  assert(s_manual_cx>512 && s_manual_cy<380);
  Xbox_D3D_ManualZoom(1);
  Xbox_D3D_ManualZoom(1);
  Xbox_D3D_ManualZoom(1);
  Xbox_D3D_ManualZoom(1);
  assert(s_manual_zoom_level==4);
  for(int i=0;i<2500;++i)Xbox_D3D_ManualPan(128,128);
  assert(s_manual_cx<=1024-1024.0f/8 && s_manual_cy>=760.0f/8);
  for(int i=0;i<2500;++i)Xbox_D3D_ManualPan(-128,-128);
  assert(s_manual_cx>=1024.0f/8 && s_manual_cy<=760-760.0f/8);
  assert(Xbox_D3D_ManualPage(3)); // turns page and recenters view
  assert(s_manual_zoom_level==0 && s_manual_cx==512 && s_manual_cy==380);
  Xbox_D3D_ManualZoom(-1);assert(s_manual_zoom_level==0);
  Xbox_D3D_ManualResetView();
  Xbox_D3D_ManualClose();
  assert(!s_manual_tex&&!s_manual_view);
  printf("R72 zoom/pan/page-reset/bounds and texture loading PASS\\n");
}
