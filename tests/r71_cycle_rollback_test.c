#include "port/player.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void) {
  player_cycle_clear();
  player_cycle_request(0,PSN_CYCLE_WEAPON,-1);
  player_cycle_request(2,PSN_CYCLE_WEAPON,-1);
  player_cycle_request(0,PSN_CYCLE_WEAPON,+1);
  assert(player_cycle_pending(0,PSN_CYCLE_WEAPON)==0);
  assert(player_cycle_pending(2,PSN_CYCLE_WEAPON)==-1);
  player_cycle_request(0,PSN_CYCLE_WEAPON,-1);
  PlayerCycleState before={0},after={0};
  player_cycle_get_state(&before);
  for(int i=0;i<4;++i) player_cycle_age();
  player_cycle_clear();
  player_cycle_set_state(&before);
  player_cycle_get_state(&after);
  assert(memcmp(&before,&after,sizeof(before))==0);
  assert(player_cycle_pending(0,PSN_CYCLE_WEAPON)==-1);
  assert(player_cycle_pending(2,PSN_CYCLE_WEAPON)==-1);
  for(int i=0;i<11;++i)player_cycle_age();
  assert(player_cycle_pending(0,PSN_CYCLE_WEAPON)==-1);
  player_cycle_age();
  assert(player_cycle_pending(0,PSN_CYCLE_WEAPON)==0);
  assert(player_cycle_pending(2,PSN_CYCLE_WEAPON)==0);
  puts("R71 reverse-weapon requests: save, restore, per-player isolation and TTL PASS");
  return 0;
}
