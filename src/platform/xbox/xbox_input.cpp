#include "xbox_platform.h"

#define NUM_PORTS 4
#define THRESH 40
static HANDLE s_pads[NUM_PORTS];
static uint16_t s_joy[2];
static bool s_right_thumb[2];
static uint16_t s_gamejoy[2];
static uint16_t s_menujoy[2];
static uint8_t s_manual_buttons[2];
static int s_manual_pan_x[2],s_manual_pan_y[2];

static bool s_exit = false;

extern "C" void Xbox_Input_Init(void) {
  ZeroMemory(s_pads, sizeof(s_pads));
  ZeroMemory(s_joy, sizeof(s_joy));
  ZeroMemory(s_right_thumb, sizeof(s_right_thumb));
  ZeroMemory(s_gamejoy, sizeof(s_gamejoy));
  ZeroMemory(s_menujoy, sizeof(s_menujoy));
  ZeroMemory(s_manual_buttons, sizeof(s_manual_buttons));
  ZeroMemory(s_manual_pan_x, sizeof(s_manual_pan_x));
  ZeroMemory(s_manual_pan_y, sizeof(s_manual_pan_y));
  XDEVICE_PREALLOC_TYPE p[] = {{XDEVICE_TYPE_GAMEPAD, NUM_PORTS}};
  XInitDevices(1, p);
  Sleep(50);
  DWORD ins = 0, rem = 0;
  XGetDeviceChanges(XDEVICE_TYPE_GAMEPAD, &ins, &rem);
  for (DWORD i = 0; i < NUM_PORTS; ++i)
    if (ins & (1u << i)) s_pads[i] = XInputOpen(XDEVICE_TYPE_GAMEPAD, i, XDEVICE_NO_SLOT, NULL);
}

extern "C" void Xbox_Input_Shutdown(void) {
  for (int i = 0; i < NUM_PORTS; ++i) if (s_pads[i]) XInputClose(s_pads[i]);
  ZeroMemory(s_pads, sizeof(s_pads));
  ZeroMemory(s_right_thumb, sizeof(s_right_thumb));
}

extern "C" void Xbox_Input_Poll(void) {
  DWORD ins = 0, rem = 0;
  if (XGetDeviceChanges(XDEVICE_TYPE_GAMEPAD, &ins, &rem)) {
    for (DWORD i = 0; i < NUM_PORTS; ++i) {
      if ((rem & (1u << i)) && s_pads[i]) { XInputClose(s_pads[i]); s_pads[i] = NULL; }
      if (ins & (1u << i)) s_pads[i] = XInputOpen(XDEVICE_TYPE_GAMEPAD, i, XDEVICE_NO_SLOT, NULL);
    }
  }
  s_joy[0] = s_joy[1] = 0;
  s_gamejoy[0] = s_gamejoy[1] = 0;
  s_menujoy[0] = s_menujoy[1] = 0;
  s_manual_buttons[0] = s_manual_buttons[1] = 0;
  s_manual_pan_x[0]=s_manual_pan_x[1]=0;
  s_manual_pan_y[0]=s_manual_pan_y[1]=0;
  s_right_thumb[0] = s_right_thumb[1] = false;
  s_exit = false;
  int player = 0;
  for (int port = 0; port < NUM_PORTS && player < 2; ++port) {
    if (!s_pads[port]) continue;
    XINPUT_STATE st; ZeroMemory(&st, sizeof(st));
    if (XInputGetState(s_pads[port], &st) != ERROR_SUCCESS) continue;
    WORD b = st.Gamepad.wButtons;
    BYTE *a = st.Gamepad.bAnalogButtons;
    const bool right_thumb = (b & XINPUT_GAMEPAD_RIGHT_THUMB) != 0;
    uint16_t j = 0;
    if (b & XINPUT_GAMEPAD_DPAD_UP) j |= 1u << 4;
    if (b & XINPUT_GAMEPAD_DPAD_DOWN) j |= 1u << 5;
    if (b & XINPUT_GAMEPAD_DPAD_LEFT) j |= 1u << 6;
    if (b & XINPUT_GAMEPAD_DPAD_RIGHT) j |= 1u << 7;
    if (b & XINPUT_GAMEPAD_START) j |= 1u << 3;
    if (b & XINPUT_GAMEPAD_BACK) j |= 1u << 2;
    if (a[XINPUT_GAMEPAD_A] > THRESH) j |= 1u << 0; /* SNES B */
    if (a[XINPUT_GAMEPAD_X] > THRESH) j |= 1u << 1; /* SNES Y */
    if (a[XINPUT_GAMEPAD_B] > THRESH) j |= 1u << 8; /* SNES A */
    if (a[XINPUT_GAMEPAD_Y] > THRESH) j |= 1u << 9; /* SNES X */
    // Keep the original shoulder codes in the FRONTEND input stream for
    // existing online menus. Gameplay strips both and maps Black to radar.
    if (a[XINPUT_GAMEPAD_LEFT_TRIGGER] > THRESH) j |= 1u << 10;
    if (a[XINPUT_GAMEPAD_RIGHT_TRIGGER] > THRESH) j |= 1u << 11;
    // Bit 12 is sent through the deterministic pad stream as "previous weapon".
    // It is deliberately NOT sent to the SNES controller (which has 12 keys).
    // Bit 13 is a local raw signal for RT, converted to SNES B in game input.
    if (a[XINPUT_GAMEPAD_LEFT_TRIGGER] > THRESH) j |= 1u << 12;
    if (a[XINPUT_GAMEPAD_RIGHT_TRIGGER] > THRESH) j |= 1u << 13;
    uint8_t manual=0;
    if (a[XINPUT_GAMEPAD_LEFT_TRIGGER] > THRESH) manual |= 1u;
    if (a[XINPUT_GAMEPAD_RIGHT_TRIGGER] > THRESH) manual |= 2u;
    if (a[XINPUT_GAMEPAD_B] > THRESH || (b & XINPUT_GAMEPAD_BACK)) manual |= 4u;
    if (b & XINPUT_GAMEPAD_START) manual |= 8u;
    // R72 manual-only controls. No bits below reach the native SNES core
    // or deterministic netplay pad stream.
    if(a[XINPUT_GAMEPAD_WHITE]>THRESH)manual |= 16u;
    if(a[XINPUT_GAMEPAD_BLACK]>THRESH)manual |= 32u;
    if(a[XINPUT_GAMEPAD_X]>THRESH)manual |= 64u; // reset view
    s_manual_buttons[player]=manual;
    int px=0,py=0;
    if(st.Gamepad.sThumbLX > 7500 || st.Gamepad.sThumbLX < -7500)
      px=(int)st.Gamepad.sThumbLX/256;
    if(st.Gamepad.sThumbLY > 7500 || st.Gamepad.sThumbLY < -7500)
      py=(int)st.Gamepad.sThumbLY/256;
    if(b & XINPUT_GAMEPAD_DPAD_LEFT)px=-128;
    if(b & XINPUT_GAMEPAD_DPAD_RIGHT)px=128;
    if(b & XINPUT_GAMEPAD_DPAD_UP)py=128;
    if(b & XINPUT_GAMEPAD_DPAD_DOWN)py=-128;
    s_manual_pan_x[player]=px;
    s_manual_pan_y[player]=py;
    /* Left stick also drives the d-pad. */
    if (st.Gamepad.sThumbLY > 12000) j |= 1u << 4;
    if (st.Gamepad.sThumbLY < -12000) j |= 1u << 5;
    if (st.Gamepad.sThumbLX < -12000) j |= 1u << 6;
    if (st.Gamepad.sThumbLX > 12000) j |= 1u << 7;
    // R49.8 system chords. START+BACK remains the deterministic shared pause.
    // LT+RT+Right-Stick-click is a second deterministic out-of-band pad bit:
    // online/online-solo code consumes it as "leave session and return to the
    // ZAMN title" on the same logical frame on both peers.  Neither chord is
    // ever forwarded to the SNES core.
    const bool start_back = (b & XINPUT_GAMEPAD_START) && (b & XINPUT_GAMEPAD_BACK);
    const bool session_exit =
        a[XINPUT_GAMEPAD_LEFT_TRIGGER] > THRESH &&
        a[XINPUT_GAMEPAD_RIGHT_TRIGGER] > THRESH &&
        (b & XINPUT_GAMEPAD_RIGHT_THUMB);
    // Preserve the old emergency application-exit chord, but require START +
    // BACK as well so the new session-exit chord cannot terminate the XBE.
    const bool hard_exit = start_back && session_exit;
    if (hard_exit) {
      s_exit = true;
    } else if (session_exit) {
      j &= (uint16_t)~((1u << 10) | (1u << 11) | (1u << 12) | (1u << 13));
      j |= 0x4000u; // ZAMN_PAD_SYS_SESSION_EXIT.
    } else if (start_back) {
      j &= (uint16_t)~((1u << 2) | (1u << 3));
      j |= 0x8000u; // ZAMN_PAD_SYS_PAUSE.
    }
    s_right_thumb[player] = right_thumb;
    s_joy[player] = j;
    // R77: Original ROM's intermediate menus accept the same select action
    // provided by physical X (SNES Y). Alias physical A to that select bit
    // ONLY in the ROM menu input stream. The native title/online menus retain
    // their original input mapping; actual gameplay never receives this alias.
    s_menujoy[player] = (uint16_t)(j | ((a[XINPUT_GAMEPAD_A] > THRESH) ? (1u<<1) : 0u));
    // Physical A remains frontend "select" only; RT produces SNES B
    // (next weapon) during gameplay. Reverse travels on bit 12.
    s_gamejoy[player] = (uint16_t)((j & ~((1u<<0)|(1u<<10)|(1u<<11)|(1u<<13))) |
                          ((j & (1u<<13)) ? (1u<<0) : 0u) |
                          ((a[XINPUT_GAMEPAD_BLACK] > THRESH) ? (1u<<10) : 0u));
    player++;
  }
}

extern "C" uint16_t Xbox_Input_GetJoypad(int p) { return (p >= 0 && p < 2) ? s_joy[p] : 0; }
extern "C" bool Xbox_Input_RightThumbDown(int p) { return (p >= 0 && p < 2) ? s_right_thumb[p] : false; }
extern "C" bool Xbox_Input_ExitRequested(void) { return s_exit; }

extern "C" uint16_t Xbox_Input_GetMenuJoypad(int p) {
  return (p>=0 && p<2)?s_menujoy[p]:0;
}
extern "C" uint16_t Xbox_Input_GetGameplayJoypad(int p) {
  return (p>=0 && p<2)?s_gamejoy[p]:0;
}
extern "C" uint8_t Xbox_Input_ManualButtons(int p) {
  return (p>=0 && p<2)?s_manual_buttons[p]:0;
}
extern "C" int Xbox_Input_ManualPanX(int p) {
  return (p>=0 && p<2)?s_manual_pan_x[p]:0;
}
extern "C" int Xbox_Input_ManualPanY(int p) {
  return (p>=0 && p<2)?s_manual_pan_y[p]:0;
}
