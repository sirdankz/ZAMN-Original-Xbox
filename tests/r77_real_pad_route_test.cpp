#include "platform/xbox/xbox_input.cpp"
#include "platform/xbox/xbox_title_online.h"
#include <cassert>
#include <cstdio>

static void press(unsigned which) {
  memset(&g_fake_input,0,sizeof(g_fake_input));
  g_fake_input.Gamepad.bAnalogButtons[which]=255;
  Xbox_Input_Poll();
}
static uint16_t choose(XboxTitleMenu& m, bool titleReady, unsigned level, bool hud) {
  const bool frontend=!m.released;
  const bool game=!frontend && Xbox_Input_StageIsPlayable(level,hud);
  const uint16_t v=frontend?Xbox_Input_GetJoypad(0):
    (game?Xbox_Input_GetGameplayJoypad(0):Xbox_Input_GetMenuJoypad(0));
  m.Input(titleReady && !Xbox_Input_StageIsPlayable(level,hud),v);
  return v;
}
int main() {
  s_pads[0]=(HANDLE)1;
  XboxTitleMenu m={}; m.Reset();
  memset(&g_fake_input,0,sizeof(g_fake_input)); Xbox_Input_Poll();
  choose(m,true,1,false);
  press(XINPUT_GAMEPAD_A);
  assert(choose(m,true,1,false)&1); assert(m.released);
  // ROM interstitial: A must act like the physical X select, while still
  // preserving the SNES B bit for older intermediate menus.
  press(XINPUT_GAMEPAD_A);
  assert((choose(m,false,1,false)&3)==3);
  press(XINPUT_GAMEPAD_X);
  assert((choose(m,false,1,false)&2)==2);
  // ROM title-yield detection may remain true with player in a real stage:
  // the stage HUD, not title yield, selects the gameplay input stream.
  press(XINPUT_GAMEPAD_A);
  assert((choose(m,true,1,true)&1)==0);
  press(XINPUT_GAMEPAD_LEFT_TRIGGER);
  uint16_t pad=choose(m,true,1,true);
  assert((pad&(1u<<12)) && !(pad&((1u<<10)|(1u<<11))));
  press(XINPUT_GAMEPAD_RIGHT_TRIGGER);
  pad=choose(m,true,1,true);
  assert((pad&1u) && !(pad&((1u<<10)|(1u<<11)|(1u<<12))));
  press(XINPUT_GAMEPAD_BLACK);
  pad=choose(m,true,1,true);
  assert((pad&(1u<<10)) && !(pad&((1u<<0)|(1u<<11)|(1u<<12))));
  assert(!Xbox_Input_StageIsPlayable(1,false));
  assert(!Xbox_Input_StageIsPlayable(0,true));
  assert(!Xbox_Input_StageIsPlayable(57,true));
  puts("R77 routing: main A / ROM menu A / gameplay LT RT Black and A suppressed PASS");
}
