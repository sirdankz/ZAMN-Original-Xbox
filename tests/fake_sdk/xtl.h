#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <vector>
using DWORD=unsigned long;using WORD=unsigned short;using BYTE=unsigned char;using HANDLE=void*;using HRESULT=long;using BOOL=int;
#define FALSE 0
#define TRUE 1
#define ERROR_SUCCESS 0
#define FAILED(a) ((a)<0)
#define SUCCEEDED(a) ((a)>=0)
#define S_OK 0
#define E_FAIL (-1)
#define ZeroMemory(p,n) memset(p,0,n)
#define D3D_SDK_VERSION 1
#define D3DFVF_XYZRHW 1
#define D3DFVF_TEX1 2
#define D3DFMT_X8R8G8B8 1
#define D3DFMT_LIN_X8R8G8B8 2
#define D3DPOOL_DEFAULT 1
#define D3DPRESENTFLAG_PROGRESSIVE 1
#define D3DPRESENTFLAG_WIDESCREEN 2
#define D3DPRESENTFLAG_INTERLACED 4
#define D3DMULTISAMPLE_NONE 0
#define D3DSWAPEFFECT_DISCARD 0
#define D3DPRESENT_INTERVAL_ONE 1
#define D3DDEVTYPE_HAL 1
#define D3DCREATE_HARDWARE_VERTEXPROCESSING 1
#define D3DRS_LIGHTING 1
#define D3DRS_ZENABLE 2
#define D3DRS_CULLMODE 3
#define D3DCULL_NONE 0
#define D3DTSS_COLOROP 1
#define D3DTSS_COLORARG1 2
#define D3DTSS_ALPHAOP 3
#define D3DTSS_MINFILTER 4
#define D3DTSS_MAGFILTER 5
#define D3DTSS_MIPFILTER 6
#define D3DTSS_ADDRESSU 7
#define D3DTSS_ADDRESSV 8
#define D3DTOP_SELECTARG1 1
#define D3DTOP_DISABLE 0
#define D3DTA_TEXTURE 0
#define D3DTEXF_POINT 0
#define D3DTEXF_NONE 0
#define D3DTADDRESS_CLAMP 0
#define D3DCLEAR_TARGET 0
#define D3DPT_TRIANGLESTRIP 0
#define D3DCOLOR_XRGB(r,g,b) 0
#define XC_VIDEO_FLAGS_WIDESCREEN 1
#define XC_VIDEO_FLAGS_HDTV_720p 2
#define XC_VIDEO_FLAGS_HDTV_480p 4
#define XC_AV_PACK_HDTV 1
#define XDEVICE_TYPE_GAMEPAD 1
#define XDEVICE_NO_SLOT 0
#define XINPUT_GAMEPAD_RIGHT_THUMB 0x80
#define XINPUT_GAMEPAD_DPAD_UP 1
#define XINPUT_GAMEPAD_DPAD_DOWN 2
#define XINPUT_GAMEPAD_DPAD_LEFT 4
#define XINPUT_GAMEPAD_DPAD_RIGHT 8
#define XINPUT_GAMEPAD_START 16
#define XINPUT_GAMEPAD_BACK 32
#define XINPUT_GAMEPAD_A 0
#define XINPUT_GAMEPAD_B 1
#define XINPUT_GAMEPAD_X 2
#define XINPUT_GAMEPAD_Y 3
#define XINPUT_GAMEPAD_LEFT_TRIGGER 4
#define XINPUT_GAMEPAD_RIGHT_TRIGGER 5
#define XINPUT_GAMEPAD_BLACK 6
#define XINPUT_GAMEPAD_WHITE 7
#define D3DTEXF_LINEAR 1
struct D3DLOCKED_RECT{ void* pBits;int Pitch; };
struct D3DPRESENT_PARAMETERS{int BackBufferWidth,BackBufferHeight,BackBufferFormat,BackBufferCount,MultiSampleType,SwapEffect,Windowed,EnableAutoDepthStencil,FullScreen_RefreshRateInHz,FullScreen_PresentationInterval;DWORD Flags;};
struct FakeTexture {
  int width,height;std::vector<unsigned char> bytes;
  FakeTexture(int w,int h):width(w),height(h),bytes(w*h*4){}
  HRESULT LockRect(int,D3DLOCKED_RECT* lr,void*,DWORD){lr->pBits=bytes.data();lr->Pitch=width*4;return 0;}
  void UnlockRect(int){}
  void Release(){delete this;}
};
struct FakeDevice { HRESULT CreateTexture(int w,int h,int,int,int,int,FakeTexture** ptr){*ptr=new FakeTexture(w,h);return 0;}void SetRenderState(int,int){}void SetTextureStageState(int,int,int){}void SetTexture(int,FakeTexture*){}void SetVertexShader(int){}void DrawPrimitiveUP(int,int,void*,int){}HRESULT BeginScene(){return 0;}void EndScene(){}void Clear(int,void*,int,int,float,int){}void Present(void*,void*,void*,void*){}void Release(){} };
struct FakeDirect3D { HRESULT CreateDevice(int,int,void*,int,D3DPRESENT_PARAMETERS*,FakeDevice**){return 0;}void Release(){} };
using LPDIRECT3D8=FakeDirect3D*;using LPDIRECT3DDEVICE8=FakeDevice*;using LPDIRECT3DTEXTURE8=FakeTexture*;
inline FakeDirect3D* Direct3DCreate8(int){return nullptr;}
inline DWORD XGetVideoFlags(){return 0;}inline DWORD XGetAVPack(){return 0;}
struct XDEVICE_PREALLOC_TYPE{int t,p;};struct FAKE_GAMEPAD{WORD wButtons; BYTE bAnalogButtons[16];short sThumbLY,sThumbLX;};struct XINPUT_STATE{FAKE_GAMEPAD Gamepad;};
inline XINPUT_STATE g_fake_input = {};
inline void XInitDevices(int,XDEVICE_PREALLOC_TYPE*){}inline int XGetDeviceChanges(int,DWORD*,DWORD*){return 0;}inline HANDLE XInputOpen(int,int,int,void*){return nullptr;}inline void XInputClose(HANDLE){}inline int XInputGetState(HANDLE,XINPUT_STATE* out){*out=g_fake_input;return 0;}inline void Sleep(unsigned){}
