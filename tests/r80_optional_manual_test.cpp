#include "platform/xbox/xbox_platform.h"
#include <cassert>
#include <cstdio>
#include <cstring>
static unsigned provided;
static int malformed=-1;
static FILE* TestManualFopen(const char* name,const char*) {
 const char* leaf=strstr(name,"spread");int page=-1;
 if(!leaf || sscanf(leaf,"spread%d.rgb565",&page)!=1 || page<0 || page>=10 || !(provided&(1u<<page)))return nullptr;
 FILE* f=tmpfile();assert(f);
 const long size=page==malformed?16:1556480;
 assert(fseek(f,size-1,SEEK_SET)==0);assert(fputc(0,f)!=EOF);rewind(f);return f;
}
#define fopen TestManualFopen
#include "platform/xbox/xbox_d3d.cpp"
#undef fopen
extern "C" void Xbox_Log(const char*,...){}
extern "C" void Xbox_Netplay_DrawText(uint8_t*,int,int,int,const char*,int,uint32_t){}
int main(){
 FakeDevice dev;s_dev=&dev;
 provided=0;assert(!Xbox_D3D_ManualOpen(0));assert(!s_manual_tex&&!s_manual_view);
 provided=1;assert(!Xbox_D3D_ManualOpen(0));assert(!s_manual_tex&&!s_manual_view);
 provided=1023u&~(1u<<9);assert(!Xbox_D3D_ManualOpen(0));assert(!s_manual_tex&&!s_manual_view);
 provided=1023;malformed=6;assert(!Xbox_D3D_ManualOpen(0));assert(!s_manual_tex&&!s_manual_view);
 malformed=-1;assert(Xbox_D3D_ManualOpen(0));assert(s_manual_tex&&s_manual_view);
 assert(Xbox_D3D_ManualPage(9));Xbox_D3D_ManualClose();assert(!s_manual_tex&&!s_manual_view);
 provided=0;assert(!Xbox_D3D_ManualOpen(0));assert(!s_manual_tex&&!s_manual_view);
 puts("Optional manual: missing, partial, malformed => no action; complete synthetic files => reader works PASS");
}
