// The runner extracts unchanged production functions into r49_production.inc.
// Only Xbox services and the simulation/snapshot payload are mocked here.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <intrin.h>
#include "netplay/netplay_core.h"
#include "platform/xbox/xbox_netplay.h"
#define __builtin_readcyclecounter __rdtsc
#define ZNP_ROLLBACK_MAX 1
static ZnpCore s_core;
static XboxNetplayStats s_stats;
static bool lan;
static bool host_route_is_confirmed_lan(bool) { return lan; }
void Xbox_Log(const char*,...) {}
void Xbox_Netplay_ReportFatal(uint8_t, uint32_t f) { s_core.fault=1;s_core.fault_frame=f; }
struct ZamnNativeRuntime { uint64_t state; uint32_t frame; uint16_t pad[2]; bool suppressed; };
struct ZamnFrameResult {};
static bool save_fail,load_fail,frame_fail;
int zamn_runtime_snapshot_size(ZamnNativeRuntime*) { return sizeof(ZamnNativeRuntime); }
int zamn_runtime_snapshot_needed_size(ZamnNativeRuntime* r) { return zamn_runtime_snapshot_size(r); }
bool zamn_runtime_save_snapshot(ZamnNativeRuntime* r,uint8_t* d,int cap,int* used) {
  if(save_fail || cap<(int)sizeof(*r))return false;
  memcpy(d,r,sizeof(*r));*used=sizeof(*r);return true;
}
bool zamn_runtime_load_snapshot(ZamnNativeRuntime* r,const uint8_t* d,int size) {
  if(load_fail || size!=(int)sizeof(*r))return false;memcpy(r,d,sizeof(*r));return true;
}
void zamn_runtime_set_replay_suppressed(ZamnNativeRuntime* r,bool s) { r->suppressed=s; }
void zamn_runtime_set_pad(ZamnNativeRuntime* r,int p,uint16_t v) { r->pad[p]=v; }
uint64_t step(uint64_t s,unsigned f,uint16_t a,uint16_t b) {return (s^((uint64_t)a<<16)^b^f)*UINT64_C(1099511628211);}
bool zamn_runtime_frame(ZamnNativeRuntime* r,void*,int,ZamnFrameResult*) {
  if(frame_fail)return false;r->state=step(r->state,r->frame++,r->pad[0],r->pad[1]);return true;
}
uint64_t zamn_runtime_state_hash(ZamnNativeRuntime* r) {return r->state;}
#include "r49_production.inc"
#include "platform/xbox/xbox_title_online.h"
static unsigned seed=49;
static unsigned random32() { seed=seed*1664525u+1013904223u;return seed; }
static uint16_t local(unsigned f) {return (uint16_t)(f*137u);}
static uint16_t remote(unsigned f) {return (uint16_t)((f/3u)*319u);}
static void reset(unsigned win) {
  memset(&s_stats,0,sizeof(s_stats));s_stats.active=1;s_rollback_enabled=true;
  znp_core_init(&s_core,0,0);rollback_reset(0,(uint8_t)win);
  save_fail=load_fail=frame_fail=false;
}
static void accept(unsigned f) { assert(rollback_accept_remote(1,f,remote(f))); }
int main() {
  // WAN target, jitter, pure delay, LAN, manual controls and calibrated filter.
  for(unsigned rtt=180;rtt<=210;++rtt) {
    assert(auto_delay_candidate(rtt,18,false,true)==5);
    assert(auto_delay_candidate(rtt,30,false,true)==6);
    assert(auto_delay_candidate(rtt,60,false,true)==7);
    assert(auto_delay_candidate(rtt,18,false,false)==(rtt*3+50)/100);
  }
  assert(auto_delay_candidate(35,0,true,true)==1);
  assert(auto_delay_candidate(228,18,false,true)==7);
  for(int d=0;d<=12;++d)for(int rb=0;rb<=1;++rb) {
    int actual;uint8_t window;
    negotiated_pair(d,7,rb!=0,&actual,&window);
    assert(actual==d && window==rb);
    negotiated_pair(-1,5,rb!=0,&actual,&window);
    assert(window==rb && actual==(rb?4:5));
  }
  assert(clamp_delay(255)==12 && clamp_delay(-1)==0);
  assert(clamp_rollback(255)==1 && clamp_rollback(0)==0);
  XboxTitleMenu menu;menu.Reset();
  assert(menu.Input(false,0)==-1 && !menu.visible);
  assert(menu.Input(true,1)==-1); // held entry confirmation is swallowed
  menu.Input(true,0);assert(menu.Input(true,1)==0); // genuine START
  menu.Reset();menu.Input(true,0);menu.Input(true,32);menu.Input(true,0);
  assert(menu.Input(true,1)==-1 && menu.online); // ONLINE consumes confirm
  assert(menu.Input(true,1)==-1); // held A does not enter a lobby
  menu.Input(true,0);assert(menu.Input(true,1)==3); // Public
  menu.Input(true,0);menu.Input(true,32);menu.Input(true,0);
  assert(menu.Input(true,1)==1); // Host Direct
  menu.Input(true,0);menu.Input(true,32);menu.Input(true,0);
  assert(menu.Input(true,1)==2); // Join Direct
  menu.Input(true,0);menu.Input(true,256);assert(!menu.online);
  menu.Input(true,0);menu.Input(true,32);menu.Input(true,0);
  assert(menu.Input(true,1)==4); // PASSWORD
  AutoDelayFilter filter;auto_delay_reset(&filter);uint32_t median;bool changed;
  for(int i=0;i<9;++i)auto_delay_update(&filter,200,18,false,true,true,true,&median,&changed);
  assert(filter.stable==5 && filter.calibrated);

  // Sparse snapshot cost really reaches zero with authoritative input.
  ZamnNativeRuntime rt={};reset(1);assert(R48_RollbackInit(&rt,1));
  for(unsigned f=0;f<1000;++f) {
    accept(f);assert(Xbox_Netplay_RemoteInputAuthoritative(f));
    if(!Xbox_Netplay_RemoteInputAuthoritative(f))assert(R48_SaveState(&rt,f));
    znp_core_commit_frame(&s_core,f);
  }
  assert(s_r49_saves==0);R48_RollbackFree();

  // R49.2: a sparse snapshot can outlive the entire input history ring.
  reset(1);assert(R48_RollbackInit(&rt,1));
  assert(s_r48_slot_count==3); // logical RB1 still needs physical bookkeeping
  assert(R48_SaveState(&rt,147));
  s_core.frame=882;assert(!Xbox_Netplay_FrameAuthoritative(147));
  assert(R48_SaveState(&rt,882));assert(R48_LoadState(&rt,882));
  R48_RollbackFree();assert(s_r48_slot_count==0 && s_r49_saves==0);

  unsigned frames=0,replays=0;
  for(unsigned win=1;win<=1;++win)for(unsigned trial=0;trial<40;++trial) {
    rt={};reset(win);assert(R48_RollbackInit(&rt,(uint8_t)win));
    for(unsigned f=0;f<240;++f) {
      // Sparse holes and out-of-order newer packets, then bounded catch-up.
      for(unsigned q=(f>win?f-win:0);q<=f+2;++q)
        if(random32()%4==0)accept(q);
      while(!rollback_can_advance(f)) {
        if(s_oldest_prediction!=0xffffffffu)accept(s_oldest_prediction);
        else accept(f);
      }
      bool replay=false;assert(R48_ApplyPendingRollback(&rt,f,&replay));
      bool saved=!Xbox_Netplay_RemoteInputAuthoritative(f);
      if(saved)assert(R48_SaveState(&rt,f));
      // Socket drain after controller sampling can correct an earlier frame.
      if(f && random32()%3==0)accept(f-1);
      if(random32()%3==0)accept(f);
      assert(znp_core_put_input(&s_core,0,f,local(f)));
      if(!znp_core_get_input(&s_core,1,f,0))assert(rollback_mark_prediction(f,s_last_actual_peer_pad));
      assert(R48_ApplyPendingRollback(&rt,f,&replay));
      if(!Xbox_Netplay_RemoteInputAuthoritative(f) && (replay||!saved))assert(R48_SaveState(&rt,f));
      uint16_t pads[2];assert(znp_core_frame_ready(&s_core,f,pads));
      rt.pad[0]=pads[0];rt.pad[1]=pads[1];assert(zamn_runtime_frame(&rt,0,0,0));
      if((f+1)%30==0)R48_RecordHash(f,rt.state);
      znp_core_commit_frame(&s_core,f);R48_SubmitReadyHashes(f,win);
      // Published hash must equal an entirely authoritative timeline.
      uint64_t truth=0;
      for(unsigned q=0;q<=f;++q) {
        truth=step(truth,q,local(q),remote(q));
        const ZnpHashSlot* h=&s_core.local_hash[q&255];
        if(h->valid && h->frame==q)assert(h->hash==truth);
      }
      assert(!s_core.fault);++frames;
    }
    for(unsigned f=0;f<240;++f)accept(f);
    bool replay;assert(R48_ApplyPendingRollback(&rt,240,&replay));
    uint64_t truth=0;for(unsigned f=0;f<240;++f)truth=step(truth,f,local(f),remote(f));
    assert(rt.state==truth && rt.frame==240 && !rt.suppressed && !s_core.fault);
    replays+=s_stats.rollback_events;R48_RollbackFree();
  }
  // A newer packet must not permit losing an older speculative pre-frame state.
  reset(1);assert(rollback_mark_prediction(0,0));s_core.frame=1;accept(2);
  assert(!rollback_can_advance(1));assert(!Xbox_Netplay_FrameAuthoritative(2));
  accept(0);assert(rollback_can_advance(1));
  // Same-value confirmation needs no replay. Changed too-old input fails closed.
  reset(1);accept(0);s_core.frame=1;assert(rollback_mark_prediction(1,remote(1)));s_core.frame=2;accept(1);
  assert(s_rb_request==0xffffffffu);
  reset(1);assert(rollback_mark_prediction(0,7));s_core.frame=3;
  assert(!rollback_accept_remote(1,0,remote(0)) && s_core.fault);
  reset(1);s_rb_request=0;s_core.frame=3;bool replay;
  assert(!R48_ApplyPendingRollback(&rt,3,&replay) && s_core.fault);
  // Save/load failure and missing snapshot all stop rather than diverge.
  reset(1);assert(R48_RollbackInit(&rt,1));save_fail=true;assert(!R48_SaveState(&rt,0));
  save_fail=false;assert(R48_SaveState(&rt,0));load_fail=true;assert(!R48_LoadState(&rt,0));
  load_fail=false;s_rb_request=1;assert(!R48_ApplyPendingRollback(&rt,2,&replay));
  R48_RollbackFree();
  printf("PASS WAN/LAN/manual policy; zero confirmed saves; %u simulated frames, %u replays; authoritative hashes and fail-stop cases\n",frames,replays);
}
