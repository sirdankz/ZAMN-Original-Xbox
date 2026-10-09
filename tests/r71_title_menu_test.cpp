#include "platform/xbox/xbox_title_online.h"
#include <assert.h>
#include <stdio.h>
static int key(XboxTitleMenu* m,unsigned k){
  m->Input(true,0);
  return m->Input(true,(uint16_t)(1u<<k));
}
int main(){
  XboxTitleMenu m={};m.Reset();
  assert(m.Input(true,0)==-1 && m.visible);
  assert(key(&m,5)==-1 && m.selection==2); // START -> PASSWORD
  assert(key(&m,7)==-1 && m.selection==3); // PASSWORD -> MANUAL
  assert(key(&m,0)==9 && m.visible && !m.released); // A opens manual modal
  assert(key(&m,4)==-1 && m.selection==1); // manual -> ONLINE
  assert(key(&m,0)==-1 && m.online);
  assert(key(&m,8)==-1 && !m.online);
  assert(key(&m,6)==-1 && m.selection==0);
  assert(key(&m,0)==0 && m.released); // START still works
  m.Reset();m.Input(true,0);
  assert(key(&m,5)==-1 && m.selection==2);
  assert(key(&m,0)==4 && m.released); // PASSWORD unchanged
  puts("R71 title grid: START ONLINE PASSWORD GAME MANUAL PASS");
  return 0;
}
