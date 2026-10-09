#include "platform/xbox/xbox_input.cpp"
#include "platform/xbox/xbox_title_online.h"
#include <cassert>
#include <cstdio>
int main() {
  s_pads[0]=(HANDLE)1;
  // Reproduce the real log: menu released, HUD false, level zero or one.
  // Session routing must not depend on either unreliable metadata field.
  for(unsigned bits=0;bits<16;++bits) {
    memset(&g_fake_input,0,sizeof(g_fake_input));
    g_fake_input.Gamepad.bAnalogButtons[XINPUT_GAMEPAD_A]=(bits&1)?255:0;
    g_fake_input.Gamepad.bAnalogButtons[XINPUT_GAMEPAD_LEFT_TRIGGER]=(bits&2)?255:0;
    g_fake_input.Gamepad.bAnalogButtons[XINPUT_GAMEPAD_RIGHT_TRIGGER]=(bits&4)?255:0;
    g_fake_input.Gamepad.bAnalogButtons[XINPUT_GAMEPAD_BLACK]=(bits&8)?255:0;
    Xbox_Input_Poll();
    uint16_t raw=Xbox_Input_GetJoypad(0), game=Xbox_Input_GetGameplayJoypad(0);
    assert(Xbox_Input_SessionPad(true,raw,game)==raw);
    uint16_t pad=Xbox_Input_SessionPad(false,raw,game);
    assert(bool(pad&1)==bool(bits&4));
    assert(bool(pad&(1u<<12))==bool(bits&2));
    assert(bool(pad&(1u<<10))==bool(bits&8));
    assert(!(pad&((1u<<1)|(1u<<11)|(1u<<13))));
  }
  XboxTitleMenu menu={}; menu.Reset(); menu.Input(true,0);
  assert(menu.Input(true,1)==0 && menu.released);
  menu.Reset(); menu.Input(true,0);
  assert(menu.Input(true,1)==0); // A still selects START after returning.
  puts("R78 session routing: A/LT/RT/Black combinations and default main menu PASS");
}
