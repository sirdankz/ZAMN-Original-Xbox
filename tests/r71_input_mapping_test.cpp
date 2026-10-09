#include "platform/xbox/xbox_input.cpp"
#include <assert.h>
#include <stdio.h>
// The fake Xbox API populates XInputGetState from this state.
int main(){
  s_pads[0]=(HANDLE)1;
  g_fake_input.Gamepad.wButtons=0;
  memset(g_fake_input.Gamepad.bAnalogButtons,0,sizeof(g_fake_input.Gamepad.bAnalogButtons));
  g_fake_input.Gamepad.bAnalogButtons[XINPUT_GAMEPAD_A]=255;
  Xbox_Input_Poll();
  assert(Xbox_Input_GetJoypad(0)&1); // A still confirms menu selection
  assert(!(Xbox_Input_GetGameplayJoypad(0)&1)); // A no longer cycles weapons
  g_fake_input.Gamepad.bAnalogButtons[XINPUT_GAMEPAD_LEFT_TRIGGER]=255;
  g_fake_input.Gamepad.bAnalogButtons[XINPUT_GAMEPAD_RIGHT_TRIGGER]=255;
  g_fake_input.Gamepad.bAnalogButtons[XINPUT_GAMEPAD_BLACK]=255;
  Xbox_Input_Poll();
  const uint16_t menu=Xbox_Input_GetJoypad(0),game=Xbox_Input_GetGameplayJoypad(0);
  assert(menu&(1u<<0));assert(menu&(1u<<10));assert(menu&(1u<<11));
  assert((game&(1u<<12)) && (game&(1u<<10)) && (game&1));
  assert(!(game&(1u<<11)) && !(game&(1u<<13)));
  assert((Xbox_Input_ManualButtons(0)&3)==3);
  g_fake_input.Gamepad.bAnalogButtons[XINPUT_GAMEPAD_LEFT_TRIGGER]=0;
  g_fake_input.Gamepad.bAnalogButtons[XINPUT_GAMEPAD_RIGHT_TRIGGER]=0;
  g_fake_input.Gamepad.bAnalogButtons[XINPUT_GAMEPAD_BLACK]=0;
  g_fake_input.Gamepad.bAnalogButtons[XINPUT_GAMEPAD_A]=0;
  Xbox_Input_Poll();
  assert(Xbox_Input_GetGameplayJoypad(0)==0);
  puts("R71 Xbox input: A title-only, LT previous, RT next, Black radar PASS");
  return 0;
}
