#pragma once
#include <stdint.h>

// R78: controller routing follows session ownership. The R77 console log
// keeps HUD=0 throughout play, so HUD/level cannot gate the gameplay pad.
static inline uint16_t Xbox_Input_SessionPad(bool frontend, uint16_t raw, uint16_t gameplay) {
  return frontend ? raw : gameplay;
}

// Pure frontend input state; never serialized into gameplay snapshots.
// R77: Title-thread yield presence is not a valid gameplay detector: the R76
// real-console trace shows false PLAYING readings on title screens and false
// MENU readings when level actors run. The ROM's $1E88/$1E8A flags are the
// HUD/player-in-stage indicators, combined with its 1..56 level word. The
// native frontend/released state is checked separately at the routing site.
static inline bool Xbox_Input_StageIsPlayable(unsigned level, bool player_hud_active) {
  return level >= 1u && level <= 56u && player_hud_active;
}

struct XboxTitleMenu {
  int selection, online_selection;
  bool online, visible, released;
  // R75: once START/PASSWORD leaves the title, keep the gameplay input map
  // latched until a real gameplay -> title transition. The old !ready branch
  // accidentally cleared released every frame outside the title.
  bool departed_title;
  uint16_t previous;
  void Reset(uint16_t held=0) {
    selection=online_selection=0;online=visible=released=departed_title=false;previous=held;
  }
  // -1: no action; 0: START; 4: PASSWORD; 9: GAME MANUAL; 3: Public Rooms; 5: SOLO; 6: PROFILE; 7: LEADERBOARDS; 8: Direct Connect submenu.
  int Input(bool ready,uint16_t held) {
    uint16_t edge=(uint16_t)(held & ~previous);previous=held;
    if(!ready){
      visible=false;
      if(released) departed_title=true; // gameplay is running; do not switch to frontend inputs
      return -1;
    }
    if(released && departed_title){
      // The original ROM returned to its title yield (game over / retry).
      // Re-arm menu controls and swallow the held button on this first frame.
      selection=online_selection=0;online=released=departed_title=false;
      visible=false;previous=held;return -1;
    }
    if(released){visible=false;return -1;}
    if(!visible){visible=true;edge=0;}

    const bool up=(edge&(1u<<4))!=0, down=(edge&(1u<<5))!=0;
    const bool left=(edge&(1u<<6))!=0, right=(edge&(1u<<7))!=0;
    if(online) {
      // R49.9.4: two-row horizontal ONLINE menu.
      // Row 0: PUBLIC ROOMS | DIRECT CONNECT
      // Row 1: SOLO PLAY | LEADERBOARDS | PROFILE
      if(left||right) {
        if(online_selection<2) online_selection=online_selection==0?1:0;
        else if(left) online_selection=online_selection==2?4:online_selection-1;
        else online_selection=online_selection==4?2:online_selection+1;
      }
      if(up||down) {
        if(online_selection==0) online_selection=2;
        else if(online_selection==1) online_selection=3;
        else if(online_selection==2) online_selection=0;
        else online_selection=1;
      }
      if(edge&(1u<<8)){online=false;return -1;}
      if(!(edge&((1u<<0)|(1u<<3))))return -1;
      if(online_selection==0)return 3; // Public Rooms
      if(online_selection==1)return 8; // Direct Connect submenu
      if(online_selection==2)return 5; // Solo Play
      if(online_selection==3)return 7; // Leaderboards
      return 6;                        // Profile
    }

    // R49.9.4: two-row horizontal MAIN menu.
    // Row 0: START | ONLINE
    // Row 1: PASSWORD | GAME MANUAL
    if(left||right) selection = (selection & 2) | ((selection ^ 1) & 1);
    if(up||down) selection ^= 2;
    if(!(edge&((1u<<0)|(1u<<3))))return -1;
    if(selection==1){online=true;online_selection=0;return -1;}
    if(selection==3)return 9; // Modal book; remains on title after exit.
    visible=false;released=true;return selection==0?0:4;
  }
};
