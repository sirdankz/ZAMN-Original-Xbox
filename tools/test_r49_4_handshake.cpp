// Extracted production host/join handshakes, scripted peer + Xbox services.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <deque>
#include "netplay/netplay_core.h"
#include "platform/xbox/xbox_netplay.h"
typedef uint32_t DWORD;
struct sockaddr_in {uint32_t sin_family,sin_port;struct {uint32_t s_addr;} sin_addr;};
static const int ZNP_PORT=6464,ZNP_MAX_PACKET=256;
static const unsigned ZNP_AUTO_ROUTE_WARMUP_LAN_MS=750,ZNP_AUTO_ROUTE_WARMUP_WAN_MS=2500;
enum {ZNP_HELLO=1,ZNP_OFFER,ZNP_READY,ZNP_START,ZNP_START_ACK,ZNP_BOOT_READY,ZNP_BOOT_GO,ZNP_INPUT,ZNP_REJECT,ZNP_GOODBYE,ZNP_PUNCH,ZNP_PING,ZNP_PONG};
#define ZMENU_A 1
#define ZMENU_B 256
#define ZMENU_START 8
#define ZMENU_Y 512
#define ZMENU_RS 8192
#define ZMENU_LEFT 64
#define ZMENU_RIGHT 128
struct ParsedPacket {uint8_t type;uint32_t session,seq,ack;const uint8_t*payload;int payload_len;};
struct Packet {uint8_t type;uint32_t session;int size;uint8_t p[32];};
static std::deque<Packet> queue;
static bool host_test,cancel_test,fail_test,peer_ready_early;
static int prepare_begins,prepare_polls,draws,starts,boot_ready_tx,go_tx,expected_delay,expected_rb;
static DWORD now_ms=1000;
static uint32_t GetTickCount(){return now_ms;}
static uint64_t __builtin_readcyclecounter(){return 49;}
static uint32_t ntohl(uint32_t n){return n;}
static void Sleep(DWORD n){now_ms+=n;}
static bool s_public_direct=false,s_public_lan_candidate=false,s_public_have_intro=true,s_peer_known=false;
static unsigned s_public_relay_rx=0;
static sockaddr_in s_peer={},s_public_peer={};
static char s_public_room[12]="ROOM",s_public_ip[32]="IP";
static uint64_t s_rom_hash=0x985A7978F6E47187ULL;
static uint32_t s_session=0;
static XboxNetplayStats s_stats;
static ZnpCore s_core;
static bool s_rollback_enabled;
static uint8_t s_rollback_window;
static int s_mode=1;
static const int ZNP_HOST_MODE=1,ZNP_SEND_HISTORY=32;
static uint32_t s_last_hash_seen=0xffffffffu;
static const char* exit_reason_name(uint8_t){return "test";}
static bool rollback_accept_remote(uint8_t slot,uint32_t frame,uint16_t pad){return znp_core_put_input(&s_core,slot,frame,pad);}
static void Xbox_Log(const char*,...){}
static void public_diag_flush(){}
static void public_tick(){}
static bool peer_equal(const sockaddr_in&,const sockaddr_in&){return true;}
static bool host_route_is_confirmed_lan(bool){return true;}
static void local_ip_text(char*p,int){strcpy(p,"LOCAL");}
static void ipv4_text(uint32_t,char*p,int){strcpy(p,"PEER");}
static void discover_public_ip(){}
static void menu_latch_current(){}
static void menu_yield(){now_ms+=16;}
static void rollback_reset(uint8_t,uint8_t){}
static void put16(uint8_t*p,uint16_t n){p[0]=(uint8_t)n;p[1]=(uint8_t)(n>>8);}
static void put32(uint8_t*p,uint32_t n){put16(p,(uint16_t)n);put16(p+2,(uint16_t)(n>>16));}
static void put64(uint8_t*p,uint64_t n){put32(p,(uint32_t)n);put32(p+4,(uint32_t)(n>>32));}
static uint16_t get16(const uint8_t*p){return p[0]|p[1]<<8;}
static uint32_t get32(const uint8_t*p){return get16(p)|(uint32_t)get16(p+2)<<16;}
static uint64_t get64(const uint8_t*p){return get32(p)|(uint64_t)get32(p+4)<<32;}
static void push(uint8_t type,const uint8_t*p=0,int n=0){Packet q={};q.type=type;q.session=s_session;q.size=n;if(n)memcpy(q.p,p,n);queue.push_back(q);}
static int prepare(bool begin){
  if(begin){++prepare_begins;assert(prepare_begins==1);return 0;}
  ++prepare_polls;assert(!s_stats.active);if(fail_test)return -1;
  return prepare_polls>=40?100:prepare_polls*2;
}
static XboxNetplayPrepare s_prepare=prepare;
static XboxNetplayBackground s_background=0;
static bool send_peer(uint8_t type,const uint8_t*p,int n){
  if(host_test){
    if(type==ZNP_PING)push(ZNP_PONG,p,n);
    if(type==ZNP_OFFER)push(ZNP_READY);
    if(type==ZNP_START){++starts;assert(p[0]==(expected_delay<0?1:expected_delay) && p[5]==expected_rb);push(ZNP_START_ACK);push(ZNP_BOOT_READY);peer_ready_early=true;}
    if(type==ZNP_BOOT_GO){++go_tx;assert(prepare_polls>=40);assert(p[0]==(expected_delay<0?1:expected_delay) && p[2]==expected_rb);}
  }else if(type==ZNP_BOOT_READY){
    ++boot_ready_tx;assert(prepare_polls>=40);uint8_t go[3]={(uint8_t)expected_delay,(uint8_t)(expected_rb!=0),(uint8_t)expected_rb};push(ZNP_BOOT_GO,go,3);
  }
  return true;
}
static bool send_raw_to(uint8_t,uint32_t,const uint8_t*,int,const sockaddr_in&){return true;}
static int recv_one(ParsedPacket*pp,sockaddr_in*,uint8_t*storage){
  if(queue.empty())return 0;Packet q=queue.front();queue.pop_front();memcpy(storage,q.p,q.size);pp->type=q.type;pp->session=q.session;pp->payload=storage;pp->payload_len=q.size;return 1;
}
static void menu_frame(const char*,const char*,const char*,const char*,const char*,const char*){++draws;assert(draws<400);}
static uint16_t menu_pressed(){
  if(cancel_test && prepare_polls>3)return ZMENU_B;
  if(!host_test)return 0;
  if(draws==1 && expected_delay<0)return ZMENU_START|(expected_rb?ZMENU_RS:0); // queue during AUTO warmup
  // Independent settings, while request remains queued until READY/calibration.
  if(expected_delay>=0 && draws==2)return ZMENU_RIGHT|(expected_rb?ZMENU_RS:0);
  if(expected_delay>=0 && draws>=3 && draws<=2+expected_delay)return ZMENU_RIGHT;
  if(expected_delay>=0 && draws==3+expected_delay)return ZMENU_START;
  return 0;
}
#include "r494_handshake.inc"
static void reset(bool host,int d,int rb){
  queue.clear();host_test=host;expected_delay=d;expected_rb=rb;
  cancel_test=fail_test=peer_ready_early=false;
  prepare_begins=prepare_polls=draws=starts=boot_ready_tx=go_tx=0;
  now_ms=1000;s_peer_known=false;s_session=0;s_public_direct=false;s_public_lan_candidate=false;
  memset(&s_stats,0,sizeof(s_stats));
}
int main(){
  const int delays[]={-1,0,3};
  for(int rb=0;rb<=1;++rb)for(int index=0;index<3;++index){
    int d=delays[index];
    reset(true,d,rb);
    uint8_t hello[8];put64(hello,s_rom_hash);push(ZNP_HELLO,hello,8);
    // Queue 20 unrelated valid packets ahead of READY to exercise batch draining.
    for(int i=0;i<20;++i)push(ZNP_PUNCH);
    assert(wait_host_handshake(true));assert(prepare_begins==1 && starts>1 && go_tx==4 && peer_ready_early);
    assert(s_stats.active && s_core.delay==(d<0?1:d) && s_rollback_window==rb && s_core.frame==0);
    // All initial GO packets lost: production gameplay receive answers retry.
    push(ZNP_BOOT_READY);receive_inputs();assert(go_tx==5);
    if(d<0)d=1;
    reset(false,d,rb);s_session=77;
    uint8_t offer[16]={};put64(offer,s_rom_hash);offer[8]=(uint8_t)d;offer[10]=(uint8_t)rb;offer[11]=(uint8_t)rb;
    push(ZNP_OFFER,offer,16);
    uint8_t start[6]={(uint8_t)d,2,0,1,(uint8_t)rb,(uint8_t)rb};
    for(int i=0;i<15;++i)push(ZNP_START,start,6);
    sockaddr_in target={};assert(wait_join_handshake(target,true));
    assert(prepare_begins==1 && boot_ready_tx>0 && s_stats.active && s_core.delay==d && s_core.frame==0);
  }
  // A complete second invocation and cancellation/failure cannot leave gameplay active.
  for(int failure=0;failure<2;++failure){
    reset(false,0,1);cancel_test=!failure;fail_test=failure;s_session=78;
    uint8_t offer[16]={};put64(offer,s_rom_hash);offer[10]=offer[11]=1;push(ZNP_OFFER,offer,16);
    uint8_t start[6]={0,2,0,1,1,1};push(ZNP_START,start,6);
    sockaddr_in target={};assert(!wait_join_handshake(target,true));assert(!s_stats.active && boot_ready_tx==0);
  }
  puts("PASS production host/join: queued Start, backlog, AUTO RB0/RB1 and manual pairs, async prep, duplicate START, early peer READY, lost GO recovery, cancel/failure, fresh second invocation");
}
