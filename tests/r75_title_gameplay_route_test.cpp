#include "platform/xbox/xbox_input.cpp"
#include "platform/xbox/xbox_title_online.h"
#include <cassert>
#include <cstdio>

static uint16_t poll_frame(XboxTitleMenu& menu, bool title_ready) {
  Xbox_Input_Poll();
  const bool frontend=!menu.released;
  const bool playing=Xbox_Input_StageIsPlayable(title_ready?0u:1u,!title_ready);
  const uint16_t bits=frontend?Xbox_Input_GetJoypad(0):
    (playing?Xbox_Input_GetGameplayJoypad(0):Xbox_Input_GetMenuJoypad(0));
  menu.Input(title_ready,bits); // identical ordering to xbox_main.cpp
  return bits;
}
static void clear_pad() { memset(&g_fake_input,0,sizeof(g_fake_input)); }
static void press(unsigned analog) { clear_pad(); g_fake_input.Gamepad.bAnalogButtons[analog]=255; }
int main() {
  s_pads[0]=(HANDLE)1;
  XboxTitleMenu menu={};menu.Reset();
  clear_pad();(void)poll_frame(menu,true);
  // A selects START on title, and is correctly forwarded to title code.
  press(XINPUT_GAMEPAD_A);
  assert((poll_frame(menu,true)&1u)!=0 && menu.released);
  // Reproduces R71-R74 bug: the original menu reset released on !ready.
  clear_pad();(void)poll_frame(menu,false);
  assert(menu.released && menu.departed_title);
  // Exactly as on a real controller, A must no longer rotate a weapon.
  press(XINPUT_GAMEPAD_A);
  assert((poll_frame(menu,false)&1u)==0 && menu.released);
  // LT = reverse weapon request only; old radar bits must not leak.
  press(XINPUT_GAMEPAD_LEFT_TRIGGER);
  uint16_t p=poll_frame(menu,false);
  assert((p&(1u<<12)) && !(p&((1u<<0)|(1u<<10)|(1u<<11))));
  // RT = SNES B, i.e. next-weapon edge; no radar bits.
  press(XINPUT_GAMEPAD_RIGHT_TRIGGER);
  p=poll_frame(menu,false);
  assert((p&1u) && !(p&((1u<<10)|(1u<<11)|(1u<<12))));
  // Black = SNES L/radar, not either trigger or weapon.
  press(XINPUT_GAMEPAD_BLACK);
  p=poll_frame(menu,false);
  assert((p&(1u<<10)) && !(p&((1u<<0)|(1u<<11)|(1u<<12))));
  // Exercise every combination of real gameplay A/LT/RT/Black while title
  // remains unavailable; catches regression of the stream-selection latch.
  for(unsigned bits=0;bits<16;bits++) {
    clear_pad();
    g_fake_input.Gamepad.bAnalogButtons[XINPUT_GAMEPAD_A]=(bits&1)?255:0;
    g_fake_input.Gamepad.bAnalogButtons[XINPUT_GAMEPAD_LEFT_TRIGGER]=(bits&2)?255:0;
    g_fake_input.Gamepad.bAnalogButtons[XINPUT_GAMEPAD_RIGHT_TRIGGER]=(bits&4)?255:0;
    g_fake_input.Gamepad.bAnalogButtons[XINPUT_GAMEPAD_BLACK]=(bits&8)?255:0;
    const uint16_t v=poll_frame(menu,false);
    assert(menu.released);
    assert(((v&(1u<<0))!=0)==((bits&4)!=0));
    assert(((v&(1u<<12))!=0)==((bits&2)!=0));
    assert(((v&(1u<<10))!=0)==((bits&8)!=0));
    assert((v&(1u<<11))==0);
  }
  // After game-over when the original ROM really reaches title again,
  // discard held button and re-arm navigation; no phantom selection.
  clear_pad();(void)poll_frame(menu,false);
  assert(menu.released);
  assert(poll_frame(menu,true)==0 && !menu.released && !menu.departed_title);
  clear_pad();(void)poll_frame(menu,true);
  press(XINPUT_GAMEPAD_A);
  assert((poll_frame(menu,true)&1u)!=0 && menu.released);
  // Existing online and manual menu navigation stays intact after Reset.
  menu.Reset();clear_pad();(void)poll_frame(menu,true);
  g_fake_input.Gamepad.wButtons=XINPUT_GAMEPAD_DPAD_DOWN;
  (void)poll_frame(menu,true);assert(menu.selection==2);
  clear_pad();(void)poll_frame(menu,true);
  g_fake_input.Gamepad.wButtons=XINPUT_GAMEPAD_DPAD_RIGHT;
  (void)poll_frame(menu,true);assert(menu.selection==3);
  clear_pad();(void)poll_frame(menu,true);
  press(XINPUT_GAMEPAD_A);assert(poll_frame(menu,true)&1u);assert(!menu.released);
  puts("R75 LIVE ROUTE: title->gameplay->title, A/trigger/Black, menu/manual PASS");
  return 0;
}
