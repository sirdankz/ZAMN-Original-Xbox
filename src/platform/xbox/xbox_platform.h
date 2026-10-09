#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <xtl.h>

#define ZAMN_FB_W 512
#define ZAMN_FB_H 480
#define ZAMN_AUDIO_RATE 48000
#define ZAMN_AUDIO_CHANNELS 2
#define ZAMN_AUDIO_SAMPLES_PER_FRAME 800

#ifdef __cplusplus
extern "C" {
#endif

HRESULT Xbox_D3D_Init(void);
void Xbox_D3D_Shutdown(void);
void Xbox_D3D_BeginDraw(uint8_t **pixels, int *pitch);
void Xbox_D3D_PrepareFrame(void);
void Xbox_D3D_SetSourceRect(int x, int y, int w, int h);
void Xbox_D3D_Present(void);

HRESULT Xbox_Audio_Init(void);
void Xbox_Audio_Shutdown(void);
void Xbox_Audio_Submit(const int16_t *samples, int frames);
void Xbox_Audio_SetMuted(bool muted);

void Xbox_Input_Init(void);
void Xbox_Input_Shutdown(void);
void Xbox_Input_Poll(void);
uint16_t Xbox_Input_GetJoypad(int player);
uint16_t Xbox_Input_GetGameplayJoypad(int player);
uint16_t Xbox_Input_GetMenuJoypad(int player);
uint8_t Xbox_Input_ManualButtons(int player);
int Xbox_Input_ManualPanX(int player); // -128..128, left stick or D-pad
int Xbox_Input_ManualPanY(int player); // -128..128, up is positive

bool Xbox_D3D_ManualOpen(int spread);
bool Xbox_D3D_ManualPage(int spread);
void Xbox_D3D_ManualZoom(int direction); // White zooms in, Black zooms out
void Xbox_D3D_ManualResetView(void);      // X returns to full spread
void Xbox_D3D_ManualPan(int dx, int dy); // pan while magnified only

void Xbox_D3D_ManualClose(void);
bool Xbox_Input_RightThumbDown(int player);
bool Xbox_Input_ExitRequested(void);

#if defined(ZAMN_RELEASE_NO_DIAGNOSTICS) && !defined(ZAMN_R499_SERVICE_LOG)
#define Xbox_Log(...) ((void)0)
#define Xbox_LogFlush() ((void)0)
#else
void Xbox_Log(const char *fmt, ...);
void Xbox_LogFlush(void);
#endif

#ifdef __cplusplus
}
#endif
