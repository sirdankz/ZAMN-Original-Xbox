#include "platform/xbox/xbox_input.cpp"
#include "platform/xbox/xbox_title_online.h"
#include <cassert>
#include <cstdio>

static uint16_t route(XboxTitleMenu& m, bool title, unsigned level) {
  Xbox_Input_Poll();
  const bool frontend=!m.released;
  const bool playing=Xbox_Input_StageIsPlayable(level, !title);
  const uint16_t v=frontend?Xbox_Input_GetJoypad(0):
    (playing?Xbox_Input_GetGameplayJoypad(0):Xbox_Input_GetMenuJoypad(0));
  m.Input(title,v);
  return v;
}
static void press(unsigned a){memset(&g_fake_input,0,sizeof(g_fake_input));g_fake_input.Gamepad.bAnalogButtons[a]=255;}
static void clear(){memset(&g_fake_input,0,sizeof(g_fake_input));}
int main(){
  s_pads[0]=(HANDLE)1;
  assert(!Xbox_Input_StageIsPlayable(1,false));
  assert(!Xbox_Input_StageIsPlayable(0,true));
  assert(!Xbox_Input_StageIsPlayable(57,true));
  assert(Xbox_Input_StageIsPlayable(1,true));
  assert(Xbox_Input_StageIsPlayable(56,true));
  XboxTitleMenu menu={};menu.Reset();
  clear();route(menu,true,0);
  press(XINPUT_GAMEPAD_A);assert((route(menu,true,0)&1) && menu.released);
  // Transition through password/character/menu BEFORE a playable stage.
  clear();route(menu,false,0);
  press(XINPUT_GAMEPAD_A);assert((route(menu,false,0)&3)==3); // A aliases ROM X select
  clear();route(menu,false,0);
  press(XINPUT_GAMEPAD_X);assert((route(menu,false,0)&1)==0); // X not substituted for A
  clear();route(menu,false,1);
  press(XINPUT_GAMEPAD_A);assert((route(menu,false,1)&1)==0); // do not cycle weapon
  press(XINPUT_GAMEPAD_LEFT_TRIGGER);uint16_t v=route(menu,false,1);
  assert((v&(1u<<12)) && !(v&((1u<<0)|(1u<<10)|(1u<<11))));
  press(XINPUT_GAMEPAD_RIGHT_TRIGGER);v=route(menu,false,1);
  assert((v&1u) && !(v&((1u<<10)|(1u<<11)|(1u<<12))));
  press(XINPUT_GAMEPAD_BLACK);v=route(menu,false,1);
  assert((v&(1u<<10)) && !(v&((1u<<0)|(1u<<11)|(1u<<12))));
  // Post-game returning to ROM title, re-arm front-end buttons.
  clear();route(menu,false,1);route(menu,true,0);
  assert(!menu.released);
  clear();route(menu,true,0);
  press(XINPUT_GAMEPAD_A);assert((route(menu,true,0)&1)!=0);
  puts("R76 Xbox routes: A preserved in menu screens; gameplay LT/RT/Black only PASS");
}
