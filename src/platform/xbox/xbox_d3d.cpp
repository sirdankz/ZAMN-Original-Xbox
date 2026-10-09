#include "xbox_platform.h"
#include <xgraphics.h>
#ifdef ZAMN_R47_NETPLAY
#include "xbox_netplay.h"
#endif

static LPDIRECT3D8 s_d3d = NULL;
static LPDIRECT3DDEVICE8 s_dev = NULL;
static LPDIRECT3DTEXTURE8 s_tex[2] = { NULL, NULL };
static int s_tex_count = 0;
static int s_present_idx = 0;
static int s_locked_idx = -1;
static uint8_t s_frame[ZAMN_FB_W * ZAMN_FB_H * 4];
static int s_disp_w = 640, s_disp_h = 480;
static bool s_linear = false;
static bool s_locked = false;
static D3DLOCKED_RECT s_lr;

// R72: High-detail 1024x760 scans; 1024x832 linear texture with footer.
// Only ONE loaded page while reading; no gameplay or netplay allocations.
static LPDIRECT3DTEXTURE8 s_manual_tex = NULL;
static bool s_manual_view = false;
static const int R72_BOOK_W=1024, R72_BOOK_H=832, R72_SCAN_H=760;
static const float R72_LEVELS[5]={1.0f,1.5f,2.0f,3.0f,4.0f};
static int s_manual_zoom_level=0;
static float s_manual_cx=R72_BOOK_W*0.5f,s_manual_cy=R72_SCAN_H*0.5f;
static void R72_ClampManual(void){
  const float z=R72_LEVELS[s_manual_zoom_level];
  const float hw=(float)R72_BOOK_W/(2.0f*z),hh=(float)R72_SCAN_H/(2.0f*z);
  if(s_manual_cx<hw)s_manual_cx=hw;
  if(s_manual_cx>(float)R72_BOOK_W-hw)s_manual_cx=(float)R72_BOOK_W-hw;
  if(s_manual_cy<hh)s_manual_cy=hh;
  if(s_manual_cy>(float)R72_SCAN_H-hh)s_manual_cy=(float)R72_SCAN_H-hh;
}
extern "C" void Xbox_D3D_ManualResetView(void){
  s_manual_zoom_level=0;
  s_manual_cx=R72_BOOK_W*0.5f;s_manual_cy=R72_SCAN_H*0.5f;
}
extern "C" void Xbox_D3D_ManualZoom(int direction){
  int next=s_manual_zoom_level+(direction>0?1:(direction<0?-1:0));
  if(next<0)next=0;
  if(next>4)next=4;
  s_manual_zoom_level=next;
  R72_ClampManual();
}
extern "C" void Xbox_D3D_ManualPan(int dx,int dy){
  if(s_manual_zoom_level==0)return;
  // D-pad and left-thumb ranges are [-128,128]. Up moves toward page top.
  const float z=R72_LEVELS[s_manual_zoom_level];
  s_manual_cx+=(float)dx*(20.0f/(128.0f*z));
  s_manual_cy-=(float)dy*(20.0f/(128.0f*z));
  R72_ClampManual();
}

// R73: The ordinary Xbox menu font clips to the 512x480 game framebuffer.
// Drawing it at y=780 on a 1024x832 manual texture silently produced NO HUD.
// Keep this tiny glyph rasterizer local to the title-only manual viewer so
// the paper/footer share one GPU texture without touching gameplay or netplay.
static const uint8_t* R73_ManualGlyph(char c) {
  /* 5x7 uppercase font. Each byte uses low five bits, left to right. */
  static const uint8_t blank[7]={0,0,0,0,0,0,0};
#define G(name,a,b,c,d,e,f,g) static const uint8_t name[7]={a,b,c,d,e,f,g}
  G(A,14,17,17,31,17,17,17); G(B,30,17,17,30,17,17,30);
  G(C,14,17,16,16,16,17,14); G(D,30,17,17,17,17,17,30);
  G(E,31,16,16,30,16,16,31); G(F,31,16,16,30,16,16,16);
  G(G,14,17,16,23,17,17,15); G(H,17,17,17,31,17,17,17);
  G(I,31,4,4,4,4,4,31);      G(J,7,2,2,2,18,18,12);
  G(K,17,18,20,24,20,18,17); G(L,16,16,16,16,16,16,31);
  G(M,17,27,21,21,17,17,17); G(N,17,25,21,19,17,17,17);
  G(O,14,17,17,17,17,17,14); G(P,30,17,17,30,16,16,16);
  G(Q,14,17,17,17,21,18,13); G(R,30,17,17,30,20,18,17);
  G(S,15,16,16,14,1,1,30);   G(T,31,4,4,4,4,4,4);
  G(U,17,17,17,17,17,17,14); G(V,17,17,17,17,17,10,4);
  G(W,17,17,17,21,21,21,10); G(X,17,17,10,4,10,17,17);
  G(Y,17,17,10,4,4,4,4);     G(Z,31,1,2,4,8,16,31);
  G(N0,14,17,19,21,25,17,14); G(N1,4,12,4,4,4,4,14);
  G(N2,14,17,1,2,4,8,31);     G(N3,30,1,1,14,1,1,30);
  G(N4,2,6,10,18,31,2,2);     G(N5,31,16,16,30,1,1,30);
  G(N6,14,16,16,30,17,17,14); G(N7,31,1,2,4,8,8,8);
  G(N8,14,17,17,14,17,17,14); G(N9,14,17,17,15,1,1,14);
  G(DOT,0,0,0,0,0,12,12); G(COLON,0,12,12,0,12,12,0);
  G(DASH,0,0,0,31,0,0,0); G(GT,16,8,4,2,4,8,16);
  G(SLASH,1,2,2,4,8,8,16); G(LT,1,2,4,8,4,2,1);
  G(PERCENT,25,26,2,4,8,11,19);
#undef G
  if (c>='a'&&c<='z') c=(char)(c-'a'+'A');
  switch(c){
    case 'A':return A;case 'B':return B;case 'C':return C;case 'D':return D;
    case 'E':return E;case 'F':return F;case 'G':return G;case 'H':return H;
    case 'I':return I;case 'J':return J;case 'K':return K;case 'L':return L;
    case 'M':return M;case 'N':return N;case 'O':return O;case 'P':return P;
    case 'Q':return Q;case 'R':return R;case 'S':return S;case 'T':return T;
    case 'U':return U;case 'V':return V;case 'W':return W;case 'X':return X;
    case 'Y':return Y;case 'Z':return Z;
    case '0':return N0;case '1':return N1;case '2':return N2;case '3':return N3;
    case '4':return N4;case '5':return N5;case '6':return N6;case '7':return N7;
    case '8':return N8;case '9':return N9;case '.':return DOT;case ':':return COLON;
    case '-':return DASH;case '>':return GT;case '<':return LT;case '/':return SLASH;
    case '%':return PERCENT;
    default:return blank;
  }
}

static void R73_ManualText(uint8_t* pixels,int pitch,int x,int y,
                           const char* message,int scale,uint32_t color){
  if(!pixels || !message || pitch<R72_BOOK_W*4 || scale<=0)return;
  const uint32_t ink=0xFF000000u | (color & 0x00FFFFFFu);
  for(const char* p=message;*p;++p){
    const uint8_t* rows=R73_ManualGlyph(*p);
    for(int ry=0;ry<7;++ry)for(int rx=0;rx<5;++rx){
      if(!(rows[ry] & (1u<<(4-rx))))continue;
      for(int yy=0; yy<scale; ++yy){
        const int dy=y+ry*scale+yy;
        if(dy<0 || dy>=R72_BOOK_H)continue;
        uint32_t* dst=(uint32_t*)(pixels+dy*pitch);
        for(int xx=0;xx<scale;++xx){
          const int dx=x+rx*scale+xx;
          if(dx>=0 && dx<R72_BOOK_W)dst[dx]=ink;
        }
      }
    }
    x+=6*scale;
  }
}
// Fixed footer never participates in paper zoom/UV panning.
static void R73_ManualHUD(uint8_t* pixels,int pitch,int spread){
  if(!pixels || pitch<R72_BOOK_W*4)return;
  // 2px page/footer separator, understated warm-paper color.
  for(int y=R72_SCAN_H;y<R72_SCAN_H+2;++y){
    uint32_t* row=(uint32_t*)(pixels+y*pitch);
    for(int x=0;x<R72_BOOK_W;++x)row[x]=0xFF9B906Fu;
  }
  char page[32];snprintf(page,sizeof(page),"SPREAD %d / 10",spread+1);
  R73_ManualText(pixels,pitch,22,770,"LT PREV",3,0x00E5D9BBu);
  R73_ManualText(pixels,pitch,406,770,page,3,0x00FFFFFFu);
  R73_ManualText(pixels,pitch,855,770,"RT NEXT",3,0x00E5D9BBu);
  R73_ManualText(pixels,pitch,22,804,
      "WHITE ZOOM IN   BLACK ZOOM OUT   STICK/D-PAD PAN   X FIT   B/BACK EXIT",
      2,0x00F1F1F1u);
}

/* Source rectangle in texture texels. Legacy LakeSnes output is 512x448
   beginning at row 16; native SNES output is 256x224/239 at 0,0. */
static int s_src_x = 0, s_src_y = 16, s_src_w = 512, s_src_h = 448;

struct QuadVert { float x, y, z, rhw, u, v; };
#define ZAMN_FVF (D3DFVF_XYZRHW | D3DFVF_TEX1)

static void UploadSwizzled(void) {
  D3DLOCKED_RECT lr;
  if (!s_tex[0] || FAILED(s_tex[0]->LockRect(0, &lr, NULL, 0))) return;
  /* Swizzled fallback: source is 512x480. Copy into a zero-padded 512x512
     scratch allocation expected by the fast XG whole-texture path. */
  static uint8_t padded[512 * 512 * 4];
  memcpy(padded, s_frame, ZAMN_FB_W * ZAMN_FB_H * 4);
  memset(padded + ZAMN_FB_W * ZAMN_FB_H * 4, 0,
         512 * (512 - ZAMN_FB_H) * 4);
  XGSwizzleRect(padded, 0, NULL, lr.pBits, 512, 512, NULL, 4);
  s_tex[0]->UnlockRect(0);
  s_present_idx = 0;
}

extern "C" HRESULT Xbox_D3D_Init(void) {
  DWORD vf = XGetVideoFlags();
  DWORD av = XGetAVPack();
  DWORD pf = 0;
  bool wide = (vf & XC_VIDEO_FLAGS_WIDESCREEN) != 0;
  if (av == XC_AV_PACK_HDTV && (vf & XC_VIDEO_FLAGS_HDTV_720p)) {
    s_disp_w = 1280; s_disp_h = 720;
    pf = D3DPRESENTFLAG_PROGRESSIVE | D3DPRESENTFLAG_WIDESCREEN;
    Xbox_Log("video: 720p\n");
  } else if (vf & XC_VIDEO_FLAGS_HDTV_480p) {
    s_disp_w = 640; s_disp_h = 480;
    pf = D3DPRESENTFLAG_PROGRESSIVE | (wide ? D3DPRESENTFLAG_WIDESCREEN : 0);
    Xbox_Log("video: 480p\n");
  } else {
    s_disp_w = 640; s_disp_h = 480;
    pf = D3DPRESENTFLAG_INTERLACED | (wide ? D3DPRESENTFLAG_WIDESCREEN : 0);
    Xbox_Log("video: 480i\n");
  }

  s_d3d = Direct3DCreate8(D3D_SDK_VERSION);
  if (!s_d3d) return E_FAIL;

  D3DPRESENT_PARAMETERS pp;
  ZeroMemory(&pp, sizeof(pp));
  pp.BackBufferWidth = s_disp_w;
  pp.BackBufferHeight = s_disp_h;
  pp.BackBufferFormat = D3DFMT_X8R8G8B8;
  pp.BackBufferCount = 1;
  pp.MultiSampleType = D3DMULTISAMPLE_NONE;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.Windowed = FALSE;
  pp.EnableAutoDepthStencil = FALSE;
  pp.FullScreen_RefreshRateInHz = 60;
  pp.FullScreen_PresentationInterval = D3DPRESENT_INTERVAL_ONE;
  pp.Flags = pf;

  HRESULT hr = s_d3d->CreateDevice(0, D3DDEVTYPE_HAL, NULL,
      D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &s_dev);
  if (FAILED(hr)) return hr;

  s_dev->SetRenderState(D3DRS_LIGHTING, FALSE);
  s_dev->SetRenderState(D3DRS_ZENABLE, FALSE);
  s_dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
  s_dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
  s_dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
  s_dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
  s_dev->SetTextureStageState(0, D3DTSS_MINFILTER, D3DTEXF_POINT);
  s_dev->SetTextureStageState(0, D3DTSS_MAGFILTER, D3DTEXF_POINT);
  s_dev->SetTextureStageState(0, D3DTSS_MIPFILTER, D3DTEXF_NONE);
  s_dev->SetTextureStageState(0, D3DTSS_ADDRESSU, D3DTADDRESS_CLAMP);
  s_dev->SetTextureStageState(0, D3DTSS_ADDRESSV, D3DTADDRESS_CLAMP);

  /* R21: two linear textures. The CPU uploads into the texture the GPU did
     not use for the previous frame, after emulation is finished. This avoids
     the R20 pattern of locking the currently sampled texture before emulation
     and holding that GPU-visible allocation locked throughout the whole core. */
  hr = s_dev->CreateTexture(ZAMN_FB_W, ZAMN_FB_H, 1, 0,
                            D3DFMT_LIN_X8R8G8B8, D3DPOOL_DEFAULT, &s_tex[0]);
  if (SUCCEEDED(hr)) {
    s_linear = true;
    s_tex_count = 1;
    HRESULT hr2 = s_dev->CreateTexture(ZAMN_FB_W, ZAMN_FB_H, 1, 0,
                                       D3DFMT_LIN_X8R8G8B8, D3DPOOL_DEFAULT,
                                       &s_tex[1]);
    if (SUCCEEDED(hr2)) s_tex_count = 2;
    s_present_idx = 0;
    Xbox_Log("D3D8 init OK; framebuffer=LINEAR staged-upload textures=%d\n", s_tex_count);
    return S_OK;
  }

  Xbox_Log("linear texture create failed hr=0x%08lx; using swizzled fallback\n",
           (unsigned long)hr);
  hr = s_dev->CreateTexture(512, 512, 1, 0, D3DFMT_X8R8G8B8,
                            D3DPOOL_DEFAULT, &s_tex[0]);
  if (SUCCEEDED(hr)) {
    s_linear = false;
    s_tex_count = 1;
    s_present_idx = 0;
    Xbox_Log("D3D8 init OK; framebuffer=SWIZZLED fallback\n");
  }
  return hr;
}

extern "C" void Xbox_D3D_BeginDraw(uint8_t **pixels, int *pitch) {
  *pixels = NULL;
  *pitch = 0;
  if (!s_tex[0]) return;

  if (!s_linear) {
    *pixels = s_frame;
    *pitch = ZAMN_FB_W * 4;
    return;
  }

  /* Upload to the other linear texture when double buffering is available.
     Lock happens only after the SNES core has finished the frame. */
  const int target = (s_tex_count >= 2) ? (s_present_idx ^ 1) : s_present_idx;
  s_dev->SetTexture(0, NULL);
  if (FAILED(s_tex[target]->LockRect(0, &s_lr, NULL, 0))) return;
  s_locked = true;
  s_locked_idx = target;
  *pixels = (uint8_t*)s_lr.pBits;
  *pitch = (int)s_lr.Pitch;
}

extern "C" void Xbox_D3D_PrepareFrame(void) {
  if (!s_linear) {
    UploadSwizzled();
    return;
  }
  if (!s_locked || s_locked_idx < 0) return;
  s_tex[s_locked_idx]->UnlockRect(0);
  s_present_idx = s_locked_idx;
  s_locked_idx = -1;
  s_locked = false;
}

extern "C" void Xbox_D3D_SetSourceRect(int x, int y, int w, int h) {
  if (x < 0) x = 0;
  if (y < 0) y = 0;
  if (w < 1) w = 1;
  if (h < 1) h = 1;
  if (x + w > ZAMN_FB_W) w = ZAMN_FB_W - x;
  if (y + h > ZAMN_FB_H) h = ZAMN_FB_H - y;
  s_src_x = x; s_src_y = y; s_src_w = w; s_src_h = h;
}

extern "C" bool Xbox_D3D_ManualPage(int spread) {
  if(!s_manual_tex || spread<0 || spread>=10)return false;
  char name[80];
  snprintf(name,sizeof(name),"D:\\manual\\spread%02d.rgb565",spread);
  FILE* f=fopen(name,"rb");
  if(!f){Xbox_Log("R71MANUAL missing %s\n",name);return false;}
  // Require complete file before disturbing current texture.
  if(fseek(f,0,SEEK_END)!=0 || ftell(f)!=(long)(R72_BOOK_W*R72_SCAN_H*2) ||
     fseek(f,0,SEEK_SET)!=0){fclose(f);return false;}
  D3DLOCKED_RECT lr;
  if(FAILED(s_manual_tex->LockRect(0,&lr,NULL,0))) {fclose(f);return false;}
  uint8_t packed[R72_BOOK_W*2];
  bool ok=true;
  for(int y=0;y<R72_BOOK_H;++y){
    uint32_t* dest=(uint32_t*)((uint8_t*)lr.pBits+y*lr.Pitch);
    if(y<R72_SCAN_H){
      if(fread(packed,1,sizeof(packed),f)!=sizeof(packed)){ok=false;break;}
      for(int x=0;x<R72_BOOK_W;++x){
        const uint16_t c=(uint16_t)(packed[2*x]|(packed[2*x+1]<<8));
        const unsigned r=((c>>11)&31u),g=((c>>5)&63u),b=c&31u;
        dest[x]=0xFF000000u | ((r<<3)|(r>>2))<<16 |
                ((g<<2)|(g>>4))<<8 | ((b<<3)|(b>>2));
      }
    }else {
      for(int x=0;x<R72_BOOK_W;++x)dest[x]=0xFF141414u;
    }
  }
  fclose(f);
  if(ok)R73_ManualHUD((uint8_t*)lr.pBits,(int)lr.Pitch,spread);
  s_manual_tex->UnlockRect(0);
  if(ok)Xbox_D3D_ManualResetView();
  return ok;
}

static bool R80_ManualFilesPresent(void) {
  for(int spread=0;spread<10;++spread) {
    char name[80];
    snprintf(name,sizeof(name),"D:\\manual\\spread%02d.rgb565",spread);
    FILE* f=fopen(name,"rb");
    if(!f)return false;
    const bool valid=fseek(f,0,SEEK_END)==0 &&
      ftell(f)==(long)(R72_BOOK_W*R72_SCAN_H*2);
    fclose(f);
    if(!valid)return false;
  }
  return true;
}

extern "C" bool Xbox_D3D_ManualOpen(int spread) {
  // Missing or incomplete user-supplied manual: leave the menu untouched.
  if(!s_dev || !R80_ManualFilesPresent())return false;
  if(!s_manual_tex){
    if(FAILED(s_dev->CreateTexture(R72_BOOK_W,R72_BOOK_H,1,0,
       D3DFMT_LIN_X8R8G8B8,D3DPOOL_DEFAULT,&s_manual_tex)))return false;
  }
  if(!Xbox_D3D_ManualPage(spread)){
    s_manual_tex->Release();s_manual_tex=NULL;return false;
  }
  s_manual_view=true;
  return true;
}

extern "C" void Xbox_D3D_ManualClose(void) {
  s_manual_view=false;
  if(s_dev)s_dev->SetTexture(0,NULL);
  if(s_manual_tex){s_manual_tex->Release();s_manual_tex=NULL;}
}

extern "C" void Xbox_D3D_Present(void) {
  if (!s_dev || (!s_manual_view && !s_tex[s_present_idx])) return;
  if(s_manual_view && !s_manual_tex)return;
  if(s_manual_view){
    const float z=R72_LEVELS[s_manual_zoom_level];
    const float hw=(float)R72_BOOK_W/(2.0f*z),hh=(float)R72_SCAN_H/(2.0f*z);
    const float u0=s_manual_cx-hw,u1=s_manual_cx+hw;
    const float v0=s_manual_cy-hh,v1=s_manual_cy+hh;
    const float bookBottom=(float)s_disp_h*(440.0f/480.0f);
    // First quad: zoomable paper. Second quad: non-scrolling page hints.
    QuadVert q[4]={{0,0,0,1,u0,v0},
      {(float)s_disp_w,0,0,1,u1,v0},
      {0,bookBottom,0,1,u0,v1},
      {(float)s_disp_w,bookBottom,0,1,u1,v1}};
    QuadVert footer[4]={{0,bookBottom,0,1,0,(float)R72_SCAN_H},
      {(float)s_disp_w,bookBottom,0,1,(float)R72_BOOK_W,(float)R72_SCAN_H},
      {0,(float)s_disp_h,0,1,0,(float)R72_BOOK_H},
      {(float)s_disp_w,(float)s_disp_h,0,1,(float)R72_BOOK_W,(float)R72_BOOK_H}};
    s_dev->Clear(0,NULL,D3DCLEAR_TARGET,D3DCOLOR_XRGB(0,0,0),1.0f,0);
    if(SUCCEEDED(s_dev->BeginScene())){
      s_dev->SetTexture(0,s_manual_tex);
      s_dev->SetTextureStageState(0,D3DTSS_MINFILTER,D3DTEXF_LINEAR);
      s_dev->SetTextureStageState(0,D3DTSS_MAGFILTER,D3DTEXF_LINEAR);
      s_dev->SetVertexShader(ZAMN_FVF);
      s_dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,q,sizeof(QuadVert));
      s_dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,footer,sizeof(QuadVert));
      s_dev->SetTextureStageState(0,D3DTSS_MINFILTER,D3DTEXF_POINT);
      s_dev->SetTextureStageState(0,D3DTSS_MAGFILTER,D3DTEXF_POINT);
      s_dev->EndScene();
    }
    s_dev->Present(NULL,NULL,NULL,NULL);
    return;
  }

  float u0, u1, v0, v1;
  if (s_linear) {
    /* Xbox linear textures use texel-space coordinates. */
    u0 = (float)s_src_x;
    u1 = (float)(s_src_x + s_src_w);
    v0 = (float)s_src_y;
    v1 = (float)(s_src_y + s_src_h);
  } else {
    u0 = (float)s_src_x / 512.0f;
    u1 = (float)(s_src_x + s_src_w) / 512.0f;
    v0 = (float)s_src_y / 512.0f;
    v1 = (float)(s_src_y + s_src_h) / 512.0f;
  }

  float dh = (float)s_disp_h;
  float dw = dh * (4.0f / 3.0f);
  if (dw > (float)s_disp_w) dw = (float)s_disp_w;
  float x0 = ((float)s_disp_w - dw) * 0.5f;
  float x1 = x0 + dw;

  QuadVert q[4] = {
    {x0, 0.0f, 0.0f, 1.0f, u0, v0},
    {x1, 0.0f, 0.0f, 1.0f, u1, v0},
    {x0, (float)s_disp_h, 0.0f, 1.0f, u0, v1},
    {x1, (float)s_disp_h, 0.0f, 1.0f, u1, v1}
  };

  s_dev->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0,0,0), 1.0f, 0);
  if (SUCCEEDED(s_dev->BeginScene())) {
    s_dev->SetTexture(0, s_tex[s_present_idx]);
    s_dev->SetVertexShader(ZAMN_FVF);
    s_dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(QuadVert));
    s_dev->EndScene();
  }
  s_dev->Present(NULL, NULL, NULL, NULL);
}

extern "C" void Xbox_D3D_Shutdown(void) {
  Xbox_D3D_ManualClose();
  if (s_locked && s_locked_idx >= 0 && s_tex[s_locked_idx]) {
    s_tex[s_locked_idx]->UnlockRect(0);
  }
  s_locked = false;
  s_locked_idx = -1;
  for (int i = 0; i < 2; ++i) {
    if (s_tex[i]) { s_tex[i]->Release(); s_tex[i] = NULL; }
  }
  s_tex_count = 0;
  if (s_dev) { s_dev->Release(); s_dev = NULL; }
  if (s_d3d) { s_d3d->Release(); s_d3d = NULL; }
}
