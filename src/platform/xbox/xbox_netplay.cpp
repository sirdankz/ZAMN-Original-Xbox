#include "xbox_netplay.h"
#include "xbox_platform.h"

#include <winsockx.h>
#include <stdlib.h>

extern "C" {
#include "netplay/netplay_core.h"
}

#define ZNP_PORT 6464
#define ZNP_PROTOCOL 4
#define ZNP_BUILD 0x5A4D490Au
#define ZNP_MAGIC0 'Z'
#define ZNP_MAGIC1 'N'
#define ZNP_MAGIC2 'P'
#define ZNP_MAGIC3 '1'
#define ZNP_HEADER 24
#define ZNP_MAX_PACKET 256
#define ZNP_SEND_HISTORY 32
#define ZNP_TIMEOUT_MS 6000
#define ZNP_RESEND_LAN_MS 8
#define ZNP_RESEND_NEAR_MS 6
#define ZNP_RESEND_WAN_MS 8
#define ZNP_RESEND_SLOW_MS 10
#define ZNP_PACE_BUDGET_MS 16
#define ZNP_ROLLBACK_MAX 1
#define ZNP_AUTO_ROUTE_WARMUP_LAN_MS 750
#define ZNP_AUTO_ROUTE_WARMUP_WAN_MS 2500

enum ZnpPacketType {
  ZNP_HELLO = 1,
  ZNP_OFFER = 2,
  ZNP_READY = 3,
  ZNP_START = 4,
  ZNP_START_ACK = 5,
  ZNP_BOOT_READY = 6,
  ZNP_BOOT_GO = 7,
  ZNP_INPUT = 8,
  ZNP_REJECT = 9,
  ZNP_GOODBYE = 10,
  ZNP_PUNCH = 11,
  ZNP_PING = 12,
  ZNP_PONG = 13,
  ZNP_CONTROL = 14,
};

enum ZnpMode { ZNP_OFF = 0, ZNP_HOST_MODE = 1, ZNP_JOIN_MODE = 2 };

static SOCKET s_sock = INVALID_SOCKET;
static bool s_wsa = false;
static ZnpMode s_mode = ZNP_OFF;
static sockaddr_in s_peer;
static bool s_peer_known = false;
static uint32_t s_session = 0;
static uint32_t s_seq = 1;
static uint64_t s_rom_hash = 0;
static ZnpCore s_core;
static XboxNetplayStats s_stats;
static DWORD s_last_rx = 0;
static DWORD s_last_tx = 0;
static uint32_t s_last_hash_seen = 0xffffffffu;
// R48 LAN lockstep phase alignment.  A faster peer moves a few milliseconds
// of repeated BeginFrame waiting to just before the next controller poll.
// This samples local input later, not earlier, so it does not add an input frame.
static uint32_t s_phase_wait_ewma_x8 = 0;
static uint8_t s_phase_pace_ms = 0;
static DWORD s_phase_last_frame_tick = 0;
// R48 late-sample pipeline: for delay>=1, wait for the remote input needed
// by the current simulation frame *before* polling the physical controller.
// The local input being sampled belongs to frame+delay, so this moves network
// waiting out of input age without changing deterministic frame semantics.
static bool s_prepared_ready = false;
static uint32_t s_prepared_frame = 0;
static uint16_t s_prepared_pads[2] = {0,0};
static void close_transport(void);
static bool ensure_network_stack(void);

// R48 rollback transport state. Prediction is hold-last-remote-input. Only the
// remote slot is ever predicted; local inputs remain authoritative immediately.
struct RollbackPrediction { uint32_t frame; uint8_t valid; };
static bool s_rollback_enabled = false;
static uint8_t s_rollback_window = 0;
static RollbackPrediction s_rb_pred[ZNP_HISTORY];
static uint32_t s_oldest_prediction = 0xffffffffu;
static uint32_t s_rb_request = 0xffffffffu;
static uint32_t s_latest_actual_peer = 0xffffffffu;
static uint16_t s_last_actual_peer_pad = 0;
static uint8_t s_exit_reason = 0;
static uint32_t s_exit_reason_frame = 0;
static void rollback_reset(uint8_t delay, uint8_t window);
static char s_local_ip[32] = "UNAVAILABLE";
static char s_public_ip[32] = "UNAVAILABLE";
static bool s_local_ip_cached = false;
// R49.9.3: frontend/network efficiency cache.
static bool s_public_ip_cached = false;
static DWORD s_public_ip_cached_tick = 0;

/* R48 PUBLIC ROOMS -------------------------------------------------------
 * MK64-style directory + NAT rendezvous + direct UDP hole punching + relay
 * fallback, reduced to ZAMN's two-player protocol. Direct Play is unchanged.
 */
#define ZDIR_PORT 6467
#define ZDIR_IP "172.233.145.244"
#define ZDIR_MAX_ROOMS 8
static bool s_public_mode = false;
static bool s_public_host = false;
static bool s_public_have_self = false;
static bool s_public_have_intro = false;
static bool s_public_direct = false;
static char s_public_room[12] = {0};
static char s_public_token[24] = {0};
static char s_public_cookie[20] = {0};
static char s_public_peer_cookie[20] = {0};
static sockaddr_in s_public_peer;
static DWORD s_public_last_keep = 0;
static DWORD s_public_last_heartbeat = 0;
static DWORD s_public_last_punch = 0;
static DWORD s_public_last_direct_rx = 0;
static uint32_t s_public_relay_tx = 0;
static uint32_t s_public_relay_rx = 0;
static uint32_t s_public_direct_valid = 0;
static bool s_public_local_direct_ok = false;
static bool s_public_peer_direct_ok = false;
static unsigned s_public_local_port = 0;
static bool s_public_lan_candidate = false;

struct PublicRoomEntry {
  char id[12];
  char name[32];
  unsigned players;
  unsigned max_players;
  unsigned ruleset;
};

// R49.9 rule/profile/account metadata. Rulesets are negotiated before frame 0
// and never change while a session is active. Online profiles are server-backed
// accounts bound to a stable hashed console identity; only opaque auth tokens
// are persisted locally after login/creation.
static uint8_t s_ruleset = ZAMN_RULE_NORMAL_SAVE;
static char s_peer_profile[16] = "PLAYER";
#define R498_PROFILE_MAGIC 0x3946505au /* 'ZPF9' */
#define R498_PROFILE_SCHEMA 2u
#define R498_PROFILE_MAX 8
#define R498_PROFILE_NAME_MAX 12
#define R499_AUTH_TOKEN_MAX 32
#define R499_PASSWORD_MAX 16
struct R499ProfileEntry {
  char name[R498_PROFILE_NAME_MAX + 1];
  char token[R499_AUTH_TOKEN_MAX + 1];
  uint8_t authenticated;
  uint8_t reserved[3];
};
struct R498ProfileStore {
  uint32_t magic, schema;
  uint8_t count, active;
  uint8_t reserved[2];
  R499ProfileEntry entries[R498_PROFILE_MAX];
};
static R498ProfileStore s_profiles;
static bool s_profiles_loaded = false;
static char s_console_id[17] = {0};
// R49.9.1: once the server validates the active profile token, keep that
// authentication for this app session. Every server operation still validates
// the token itself, so menu navigation no longer reopens XNet/UDP just to prove
// the same account repeatedly.
static bool s_r499_session_auth = false;
static char s_r499_session_auth_name[R498_PROFILE_NAME_MAX + 1] = {0};

class R499BlockingAudioMute {
public:
  R499BlockingAudioMute() { Xbox_Audio_SetMuted(true); }
  ~R499BlockingAudioMute() { Xbox_Audio_SetMuted(false); }
};

#define R498_SCORE_MAGIC 0x39424c5au /* 'ZLB9' */
#define R498_SCORE_SCHEMA 2u
#define R498_LOCAL_SCORES 128
#define R499_SCOPE_SOLO 0u
#define R499_SCOPE_ONLINE 1u
struct R498LocalScore {
  uint8_t ruleset;
  uint8_t players;
  uint8_t sent;
  uint8_t scope;
  uint16_t level;
  uint16_t reserved2;
  uint32_t frames;
  char name[R498_PROFILE_NAME_MAX + 1];
  char peer[R498_PROFILE_NAME_MAX + 1];
};
struct R498ScoreStore {
  uint32_t magic, schema;
  uint32_t count;
  R498LocalScore rows[R498_LOCAL_SCORES];
};
static R498ScoreStore s_scores;
static bool s_scores_loaded = false;

struct R499ChatRow {
  uint32_t seq;
  char name[R498_PROFILE_NAME_MAX + 1];
  char text[49];
};

static bool open_socket(bool public_socket);
static bool r499_profile_auth_dialog(bool create_account,const char* user,const char* password,char* status,int status_cap);
static void r499_chat_menu(bool lobby_default);

static void put16(uint8_t* p, uint16_t v) {
  p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}
static void put32(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void put64(uint8_t* p, uint64_t v) {
  put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32));
}
static uint16_t get16(const uint8_t* p) {
  return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t get32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t get64(const uint8_t* p) {
  return (uint64_t)get32(p) | ((uint64_t)get32(p + 4) << 32);
}
static uint16_t get16be(const uint8_t* p) {
  return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}
static uint32_t get32be(const uint8_t* p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* R48 direct-play premenu -------------------------------------------------
 * This deliberately mirrors the proven MK64 OG Xbox direct-play flow:
 *   OFFLINE / HOST DIRECT / JOIN DIRECT
 *   JOIN edits the host IPv4 on-screen with the D-pad.
 *   HOST waits for P2 and only starts the synchronized timeline when A is
 *   pressed. Rendering is local and does not touch deterministic game state.
 */
#define ZMENU_A     (1u << 0)  /* Xbox A -> frontend's SNES-B bit */
#define ZMENU_B     (1u << 8)  /* Xbox B -> frontend's SNES-A bit */
#define ZMENU_START (1u << 3)
#define ZMENU_UP    (1u << 4)
#define ZMENU_DOWN  (1u << 5)
#define ZMENU_LEFT  (1u << 6)
#define ZMENU_RIGHT (1u << 7)
#define ZMENU_Y     (1u << 9)
#define ZMENU_RS    (1u << 13) /* frontend-only: right-stick click */

static uint16_t s_menu_prev = 0;
static bool s_menu_prev_rs = false;
static XboxNetplayBackground s_background = NULL;
static XboxNetplayPrepare s_prepare = NULL;

static uint16_t menu_pressed(void) {
  Xbox_Input_Poll();
  uint16_t now = Xbox_Input_GetJoypad(0);
  uint16_t edge = (uint16_t)(now & ~s_menu_prev);
  const bool rs = Xbox_Input_RightThumbDown(0);
  if (rs && !s_menu_prev_rs) edge |= ZMENU_RS;
  s_menu_prev = now;
  s_menu_prev_rs = rs;
  return edge;
}

/* R48: entering a nested menu must inherit the buttons that are already
 * held.  Otherwise the A edge that selected PUBLIC ROOMS is seen again by
 * the new screen and immediately selects HOST PUBLIC ROOM. */
static void menu_latch_current(void) {
  Xbox_Input_Poll();
  s_menu_prev = Xbox_Input_GetJoypad(0);
  s_menu_prev_rs = Xbox_Input_RightThumbDown(0);
}

static void public_diag_flush(void) {
  /* Public-room debugging happens before gameplay.  R39 intentionally keeps
   * the normal gameplay log buffered, so flush only these low-frequency menu
   * transitions to make failed room tests immediately inspectable. */
  Xbox_LogFlush();
}

static const uint8_t* font_rows(char c) {
  /* 5x7 uppercase font. Each byte uses low five bits, left to right. */
  static const uint8_t blank[7]={0,0,0,0,0,0,0};
#define G(name,a,b,c,d,e,f,g) static const uint8_t name[7]={a,b,c,d,e,f,g}
  G(A,14,17,17,31,17,17,17); G(B,30,17,17,30,17,17,30);
  G(C,14,17,16,16,16,17,14); G(D,30,17,17,17,17,17,30);
  G(E,31,16,16,30,16,16,31); G(F,31,16,16,30,16,16,16);
  G(G,14,17,16,23,17,17,15); G(H,17,17,17,31,17,17,17);
  G(I,31,4,4,4,4,4,31);      G(J,7,2,2,2,18,18,12);
  G(K,17,18,20,24,20,18,17); G(L,16,16,16,16,16,16,31);
  G(M,17,27,21,21,17,17,17); G(N,17,25,21,19,17,17,17);
  G(O,14,17,17,17,17,17,14); G(P,30,17,17,30,16,16,16);
  G(Q,14,17,17,17,21,18,13); G(R,30,17,17,30,20,18,17);
  G(S,15,16,16,14,1,1,30);   G(T,31,4,4,4,4,4,4);
  G(U,17,17,17,17,17,17,14); G(V,17,17,17,17,17,10,4);
  G(W,17,17,17,21,21,21,10); G(X,17,17,10,4,10,17,17);
  G(Y,17,17,10,4,4,4,4);     G(Z,31,1,2,4,8,16,31);
  G(N0,14,17,19,21,25,17,14); G(N1,4,12,4,4,4,4,14);
  G(N2,14,17,1,2,4,8,31);     G(N3,30,1,1,14,1,1,30);
  G(N4,2,6,10,18,31,2,2);     G(N5,31,16,16,30,1,1,30);
  G(N6,14,16,16,30,17,17,14); G(N7,31,1,2,4,8,8,8);
  G(N8,14,17,17,14,17,17,14); G(N9,14,17,17,15,1,1,14);
  G(DOT,0,0,0,0,0,12,12); G(COLON,0,12,12,0,12,12,0);
  G(DASH,0,0,0,31,0,0,0); G(GT,16,8,4,2,4,8,16);
  G(SLASH,1,2,2,4,8,8,16); G(LT,1,2,4,8,4,2,1);
  G(PERCENT,25,26,2,4,8,11,19);
#undef G
  if (c>='a'&&c<='z') c=(char)(c-'a'+'A');
  switch(c){
    case 'A':return A;case 'B':return B;case 'C':return C;case 'D':return D;
    case 'E':return E;case 'F':return F;case 'G':return G;case 'H':return H;
    case 'I':return I;case 'J':return J;case 'K':return K;case 'L':return L;
    case 'M':return M;case 'N':return N;case 'O':return O;case 'P':return P;
    case 'Q':return Q;case 'R':return R;case 'S':return S;case 'T':return T;
    case 'U':return U;case 'V':return V;case 'W':return W;case 'X':return X;
    case 'Y':return Y;case 'Z':return Z;
    case '0':return N0;case '1':return N1;case '2':return N2;case '3':return N3;
    case '4':return N4;case '5':return N5;case '6':return N6;case '7':return N7;
    case '8':return N8;case '9':return N9;case '.':return DOT;case ':':return COLON;
    case '-':return DASH;case '>':return GT;case '<':return LT;case '/':return SLASH;
    case '%':return PERCENT;
    default:return blank;
  }
}

static void menu_rect(uint8_t* pixels,int pitch,int x,int y,int w,int h,uint32_t color){
  if(!pixels||pitch<4)return;
  const int width=pitch/4<ZAMN_FB_W?pitch/4:ZAMN_FB_W;
  if(x<0){w+=x;x=0;} if(y<0){h+=y;y=0;}
  if(x+w>width)w=width-x; if(y+h>ZAMN_FB_H)h=ZAMN_FB_H-y;
  if(w<=0||h<=0)return;
  for(int yy=0;yy<h;++yy){uint32_t* row=(uint32_t*)(pixels+(y+yy)*pitch)+x;for(int xx=0;xx<w;++xx)row[xx]=color;}
}

static void menu_text(uint8_t* pixels,int pitch,int x,int y,const char* text,int scale,uint32_t color){
  if(!pixels||!text||scale<1)return;
  int ox=x;
  for(const char* p=text;*p;++p){
    if(*p=='\n'){y+=8*scale;x=ox;continue;}
    const uint8_t* rows=font_rows(*p);
    for(int ry=0;ry<7;++ry)for(int rx=0;rx<5;++rx)if(rows[ry]&(1u<<(4-rx)))
      menu_rect(pixels,pitch,x+rx*scale,y+ry*scale,scale,scale,color);
    x+=6*scale;
  }
}

void Xbox_Netplay_DrawText(uint8_t* pixels, int pitch, int x, int y, const char* text, int scale, uint32_t color) {
  menu_text(pixels,pitch,x,y,text,scale,color);
}

// R49.4: composite in cached native-size RAM, then lock/upload once.
static uint8_t s_menu_canvas[256*224*4] __attribute__((aligned(64)));
static DWORD s_menu_frame_tick=0;
static void menu_panel(uint8_t* pixels,int pitch) {
  for(int y=9;y<219;++y) {
    uint32_t* row=(uint32_t*)(pixels+y*pitch);
    for(int x=10;x<246;++x)row[x]=(row[x]>>2)&0x003f3f3fu; // 75% black
  }
  menu_rect(pixels,pitch,10,9,236,1,0x00408040u);
  menu_rect(pixels,pitch,10,218,236,1,0x00408040u);
  menu_rect(pixels,pitch,10,9,1,210,0x00408040u);
  menu_rect(pixels,pitch,245,9,1,210,0x00408040u);
}
static void menu_ui_text(uint8_t* pixels,int pitch,int x,int y,const char* text,int scale,uint32_t color) {
  if(!text)return;
  x/=2;y/=2;scale=1;
  const int first=x;
  for(const char* p=text;*p && y<212;++p) {
    if(*p!=' ' && (p==text || p[-1]==' ')) {
      int word=0;while(p[word] && p[word]!=' ' && p[word]!='\n')++word;
      if(x!=first && x+word*6>242){x=first;y+=9;}
    }
    if(x+5>=242 || *p=='\n'){x=first;y+=9;if(*p=='\n')continue;}
    if(y>=212)break;
    if(x==first && *p==' ')continue;
    char ch[2]={*p,0};menu_text(pixels,pitch,x,y,ch,scale,color);x+=6;
  }
}
static void menu_ui_rect(uint8_t* pixels,int pitch,int x,int y,int w,int h,uint32_t color) {
  menu_rect(pixels,pitch,x/2,y/2,(w+1)/2,(h+1)/2,color);
}
static void menu_begin(uint8_t** pixels,int* pitch) {
  s_menu_frame_tick=GetTickCount();*pixels=s_menu_canvas;*pitch=256*4;
  if(s_background)s_background(*pixels,*pitch);
  else memset(s_menu_canvas,0,sizeof(s_menu_canvas));
  menu_panel(*pixels,*pitch);
}
static void menu_present(void) {
  uint8_t* target=0;int pitch=0;Xbox_D3D_BeginDraw(&target,&pitch);
  if(target && pitch>=256*4)for(int y=0;y<224;++y)
    memcpy(target+y*pitch,s_menu_canvas+y*256*4,256*4);
  Xbox_D3D_SetSourceRect(0,0,256,224);
  Xbox_D3D_PrepareFrame();Xbox_D3D_Present();
  static DWORD started=0;static unsigned frames=0;
  if(!started)started=GetTickCount();
  if(++frames>=120) {
    DWORD elapsed=GetTickCount()-started;
    Xbox_Log("ZNET R49.4 LOBBY frames=%u elapsed=%lu ms native256=1\n",frames,(unsigned long)elapsed);
    started=0;frames=0;
  }
}
static void menu_yield(void) {
  // Present already waits for vblank. Sleep only if there is actual spare time.
  DWORD elapsed=GetTickCount()-s_menu_frame_tick;
  if(elapsed<16)Sleep(16-elapsed);
}

static void menu_frame(const char* title,const char* l1,const char* l2,const char* l3,const char* l4,const char* footer){
  uint8_t* pixels=0;int pitch=0;menu_begin(&pixels,&pitch);
  if(pixels){
    menu_ui_text(pixels,pitch,44,43,title?title:"ZAMN NETPLAY",2,0x00FFFFFFu);
    if(l1)menu_ui_text(pixels,pitch,56,120,l1,2,0x0068FF68u);
    if(l2)menu_ui_text(pixels,pitch,56,166,l2,2,0x0068FF68u);
    if(l3)menu_ui_text(pixels,pitch,56,212,l3,2,0x0068FF68u);
    if(l4)menu_ui_text(pixels,pitch,56,258,l4,2,0x0068FF68u);
    if(footer)menu_ui_text(pixels,pitch,42,398,footer,1,0x00C8D4DCu);
  }
  menu_present();
}


static const char* r498_ruleset_name(uint8_t r) {
  switch (r & 3u) {
    case ZAMN_RULE_NORMAL_SAVE: return "NORMAL / SAVABLE";
    case ZAMN_RULE_NORMAL_NOSAVE: return "NORMAL / NO SAVE";
    case ZAMN_RULE_HARDCORE_SAVE: return "HARDCORE / SAVABLE";
    default: return "HARDCORE / NO SAVE";
  }
}
// R49.9.5: the mode picker explains the gameplay difference without making
// compact lobby/leaderboard labels wider.
static const char* r4995_ruleset_menu_name(uint8_t r) {
  switch (r & 3u) {
    case ZAMN_RULE_NORMAL_SAVE: return "NORMAL / SAVABLE (+1 LIFE EVERY 5 LEVELS)";
    case ZAMN_RULE_NORMAL_NOSAVE: return "NORMAL / NO SAVE (+1 LIFE EVERY 5 LEVELS)";
    case ZAMN_RULE_HARDCORE_SAVE: return "HARDCORE / SAVABLE (ORIGINAL GAMEPLAY)";
    default: return "HARDCORE / NO SAVE (ORIGINAL GAMEPLAY)";
  }
}
void Xbox_Netplay_SetRuleset(uint8_t ruleset){s_ruleset=(uint8_t)(ruleset&3u);}
uint8_t Xbox_Netplay_Ruleset(void){return s_ruleset;}
const char* Xbox_Netplay_RulesetName(uint8_t ruleset){return r498_ruleset_name(ruleset);}
bool Xbox_Netplay_RulesetSavable(uint8_t ruleset){return (ruleset&1u)==0;}
bool Xbox_Netplay_RulesetNormal(uint8_t ruleset){return (ruleset&2u)==0;}

static bool r498_meta_root(char* out,int cap) {
  static char root[MAX_PATH]={0};
  if(!out||cap<4)return false;
  if(!root[0]){
    DWORD ret=XCreateSaveGame("U:\\",L"ZAMN - Online Profile",OPEN_ALWAYS,0,root,(UINT)sizeof(root));
    if(ret!=ERROR_SUCCESS)return false;
    size_t n=strlen(root);if(n&&root[n-1]!='\\'&&n+1<sizeof(root)){root[n]='\\';root[n+1]=0;}
  }
  strncpy(out,root,(size_t)cap-1);out[cap-1]=0;return true;
}

static bool r498_meta_path(const char* leaf,char* out,int cap){
  char root[MAX_PATH];if(!r498_meta_root(root,sizeof(root)))return false;
  snprintf(out,(size_t)cap,"%s%s",root,leaf);if(cap>0)out[cap-1]=0;return true;
}

static void r498_clean_name(const char* in,char* out,int cap){
  int n=0;if(out&&cap>0)out[0]=0;
  for(const char* p=in;out&&p&&*p&&n<cap-1&&n<R498_PROFILE_NAME_MAX;++p){
    char c=*p;if(c>='a'&&c<='z')c=(char)(c-'a'+'A');
    if((c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_')out[n++]=c;
  }
  if(out&&cap>0)out[n]=0;
}

static uint64_t r499_fnv64(const uint8_t* p,int n,uint64_t seed){
  uint64_t h=seed?seed:14695981039346656037ull;
  for(int i=0;i<n;++i){h^=p[i];h*=1099511628211ull;}
  return h;
}

static bool r499_console_id_text(char out[17]){
  if(s_console_id[0]){if(out)strcpy(out,s_console_id);return true;}
  bool started_here=false;
  if(!s_wsa){
    XNetStartupParams xp;memset(&xp,0,sizeof(xp));xp.cfgSizeOfStruct=sizeof(xp);xp.cfgFlags=XNET_STARTUP_BYPASS_SECURITY;
    if(XNetStartup(&xp)!=0)return false;started_here=true;
  }
  XNADDR xa;memset(&xa,0,sizeof(xa));DWORD flags=XNET_GET_XNADDR_PENDING;DWORD begin=GetTickCount();
  while(GetTickCount()-begin<3000u){flags=XNetGetTitleXnAddr(&xa);if(!(flags&XNET_GET_XNADDR_PENDING))break;Sleep(20);}
  if(started_here)XNetCleanup();
  // Never send/store the raw Ethernet address.  A salted one-way 64-bit device
  // identifier is sufficient for account binding/moderation and avoids putting
  // an IP address or raw MAC address in the service database.
  uint64_t h=0xcbf29ce484222325ull;
  static const uint8_t salt[]={0x5a,0x41,0x4d,0x4e,0x2d,0x58,0x42,0x4f,0x58,0x2d,0x52,0x34,0x39,0x39};
  h=r499_fnv64(salt,(int)sizeof(salt),h);h=r499_fnv64(xa.abEnet,6,h);
  snprintf(s_console_id,sizeof(s_console_id),"%08lX%08lX",(unsigned long)(h>>32),(unsigned long)h);
  s_console_id[16]=0;if(out)strcpy(out,s_console_id);return true;
}

static void r498_profiles_defaults(void){
  // R49.9.4: a fresh console has no implicit PLAYER/Profile 1. Online access
  // starts at CREATE PROFILE or SIGN IN / RECOVER so every visible profile is
  // a real server-backed account.
  memset(&s_profiles,0,sizeof(s_profiles));s_profiles.magic=R498_PROFILE_MAGIC;s_profiles.schema=R498_PROFILE_SCHEMA;
  s_profiles.count=0;s_profiles.active=0;s_profiles_loaded=true;
}
static void r498_profiles_load(void){
  if(s_profiles_loaded)return;r498_profiles_defaults();char path[MAX_PATH];if(!r498_meta_path("profiles-r499.dat",path,sizeof(path)))return;
  FILE* f=fopen(path,"rb");
  if(f){
    R498ProfileStore t;bool ok=fread(&t,1,sizeof(t),f)==sizeof(t);fclose(f);
    if(ok&&t.magic==R498_PROFILE_MAGIC&&t.schema==R498_PROFILE_SCHEMA&&t.count<=R498_PROFILE_MAX&&(t.count==0||t.active<t.count)){
      // Purge the old auto-generated placeholder if that is all the file has.
      if(t.count==1&&!strcmp(t.entries[0].name,"PLAYER")&&!t.entries[0].token[0]){r498_profiles_defaults();return;}
      s_profiles=t;return;
    }
  }
  // One-time local migration of R49.8 names. These are deliberately marked
  // unauthenticated; empty/PLAYER placeholders are ignored rather than turned
  // into fake visible accounts.
  char oldpath[MAX_PATH];if(!r498_meta_path("profiles.dat",oldpath,sizeof(oldpath)))return;
  struct Legacy {uint32_t magic,schema;uint8_t count,active,reserved[2];char names[8][13];};Legacy old;memset(&old,0,sizeof(old));
  f=fopen(oldpath,"rb");if(!f)return;bool ok=fread(&old,1,sizeof(old),f)==sizeof(old);fclose(f);
  if(ok&&old.count>0&&old.count<=R498_PROFILE_MAX&&old.active<old.count){
    R498ProfileStore migrated;memset(&migrated,0,sizeof(migrated));migrated.magic=R498_PROFILE_MAGIC;migrated.schema=R498_PROFILE_SCHEMA;
    for(unsigned i=0;i<old.count&&migrated.count<R498_PROFILE_MAX;++i){
      char clean[R498_PROFILE_NAME_MAX+1]={0};r498_clean_name(old.names[i],clean,sizeof(clean));
      if(!clean[0]||!strcmp(clean,"PLAYER"))continue;
      strncpy(migrated.entries[migrated.count].name,clean,R498_PROFILE_NAME_MAX);
      ++migrated.count;
    }
    migrated.active=0;s_profiles=migrated;
  }
}
static void r498_profiles_save(void){
  r498_profiles_load();char path[MAX_PATH];if(!r498_meta_path("profiles-r499.dat",path,sizeof(path)))return;FILE* f=fopen(path,"wb");if(!f)return;fwrite(&s_profiles,1,sizeof(s_profiles),f);fclose(f);
}
static R499ProfileEntry* r499_active_profile(void){r498_profiles_load();if(!s_profiles.count||s_profiles.active>=s_profiles.count)return NULL;return &s_profiles.entries[s_profiles.active];}
const char* Xbox_Netplay_ProfileName(void){R499ProfileEntry* p=r499_active_profile();return p&&p->name[0]?p->name:"NO PROFILE";}
const char* Xbox_Netplay_PeerProfileName(void){return s_peer_profile[0]?s_peer_profile:"PLAYER";}
static const char* r499_profile_token(void){R499ProfileEntry* p=r499_active_profile();return p?p->token:"";}
static bool r499_store_profile_auth(const char* user,const char* token){
  char clean[R498_PROFILE_NAME_MAX+1];r498_clean_name(user,clean,sizeof(clean));if(!clean[0]||!token||!token[0])return false;r498_profiles_load();
  int idx=-1;for(unsigned i=0;i<s_profiles.count;++i)if(!strcmp(s_profiles.entries[i].name,clean)){idx=(int)i;break;}
  if(idx<0){if(s_profiles.count>=R498_PROFILE_MAX)return false;idx=s_profiles.count++;memset(&s_profiles.entries[idx],0,sizeof(s_profiles.entries[idx]));strncpy(s_profiles.entries[idx].name,clean,R498_PROFILE_NAME_MAX);}
  strncpy(s_profiles.entries[idx].token,token,R499_AUTH_TOKEN_MAX);s_profiles.entries[idx].token[R499_AUTH_TOKEN_MAX]=0;s_profiles.entries[idx].authenticated=1;s_profiles.active=(uint8_t)idx;r498_profiles_save();
  s_r499_session_auth=true;strncpy(s_r499_session_auth_name,clean,R498_PROFILE_NAME_MAX);s_r499_session_auth_name[R498_PROFILE_NAME_MAX]=0;
  return true;
}

static const char* r498_keys(void){return "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_+ ";}
static bool r499_keyboard(const char* title,const char* label,char* out,int cap,int max_chars,bool mask){
  if(!out||cap<2)return false;char text[64]={0};int key=0;if(max_chars>cap-1)max_chars=cap-1;if(max_chars>48)max_chars=48;
  // R49.9.4: inherit the currently-held buttons.  The A/START press that
  // opened this keyboard must not become the first typed character or skip
  // directly into the password field.
  Xbox_Input_Poll();uint16_t prev=Xbox_Input_GetJoypad(0);menu_latch_current();
  for(;;){
    uint8_t* pixels=0;int pitch=0;menu_begin(&pixels,&pitch);
    if(pixels){
      menu_ui_text(pixels,pitch,44,36,title?title:"TEXT ENTRY",2,0x00FFFFFFu);
      char shown[64]={0};if(mask){size_t n=strlen(text);for(size_t i=0;i<n&&i<sizeof(shown)-1;++i)shown[i]='X';}else strncpy(shown,text,sizeof(shown)-1);
      char value[96];snprintf(value,sizeof(value),"%s: %s_",label?label:"TEXT",shown);menu_ui_text(pixels,pitch,48,84,value,1,0x00FFD35Au);
      const char* keys=r498_keys();for(int i=0;i<40;++i){int row=i/8,col=i%8,x=54+col*48,y=142+row*38;char ch[2]={keys[i],0};if(ch[0]==' ')ch[0]='_';if(i==key)menu_ui_rect(pixels,pitch,x-5,y-5,24,25,0x00404020u);menu_ui_text(pixels,pitch,x,y,ch,1,i==key?0x00FFFFFFu:0x0068FF68u);}
      menu_ui_text(pixels,pitch,42,397,"A TYPE  X DELETE  START DONE  B CANCEL",1,0x00C8D4DCu);
    }menu_present();Xbox_Input_Poll();uint16_t now=Xbox_Input_GetJoypad(0);uint16_t p=(uint16_t)(now&~prev);prev=now;int row=key/8,col=key%8;
    if(p&ZMENU_UP){row=(row+4)%5;key=row*8+col;}if(p&ZMENU_DOWN){row=(row+1)%5;key=row*8+col;}if(p&ZMENU_LEFT){col=(col+7)%8;key=row*8+col;}if(p&ZMENU_RIGHT){col=(col+1)%8;key=row*8+col;}
    if(p&(1u<<1)){size_t n=strlen(text);if(n)text[n-1]=0;}if(p&ZMENU_B)return false;
    if(p&ZMENU_A){size_t n=strlen(text);if((int)n<max_chars){char c=r498_keys()[key];if(!(c==' '&&n==0)){text[n]=c;text[n+1]=0;}}}
    if((p&ZMENU_START)&&text[0]){strncpy(out,text,(size_t)cap-1);out[cap-1]=0;return true;}menu_yield();
  }
}

void Xbox_Netplay_ProfileMenu(void){
  r498_profiles_load();int sel=s_profiles.count?s_profiles.active:0;char notice[64]="CREATE OR RECOVER AN ONLINE PROFILE";menu_latch_current();
  for(;;){
    const int action_create=(int)s_profiles.count;
    const int action_recover=(int)s_profiles.count+1;
    const int maxsel=action_recover;
    if(sel>maxsel)sel=maxsel;
    char a[72]="",b[72]="",c[72]="",d[72]="";int first=sel-1;if(first<0)first=0;if(first>maxsel-2)first=maxsel-2;if(first<0)first=0;char* rows[3]={a,b,c};
    for(int j=0;j<3;++j){
      const int i=first+j;
      if(i<(int)s_profiles.count){
        R499ProfileEntry& e=s_profiles.entries[i];
        snprintf(rows[j],71,"%c %s%s%s",i==sel?'>':' ',e.name,i==s_profiles.active?" ACTIVE":"",e.authenticated?"":" SIGN IN");
      }else if(i==action_create)snprintf(rows[j],71,"%c CREATE PROFILE",i==sel?'>':' ');
      else if(i==action_recover)snprintf(rows[j],71,"%c SIGN IN / RECOVER",i==sel?'>':' ');
    }
    snprintf(d,sizeof(d),"%s",notice);menu_frame("ONLINE PROFILE",a,b,c,d,"A SELECT   X REMOVE LOCAL   B BACK");uint16_t p=menu_pressed();
    if(p&ZMENU_B){r498_profiles_save();return;}if(p&ZMENU_UP)sel=sel?sel-1:maxsel;if(p&ZMENU_DOWN)sel=sel>=maxsel?0:sel+1;
    if((p&(1u<<1))&&sel<(int)s_profiles.count){
      for(int i=sel;i+1<(int)s_profiles.count;++i)s_profiles.entries[i]=s_profiles.entries[i+1];
      if(s_profiles.count)--s_profiles.count;
      if(!s_profiles.count)s_profiles.active=0;else if(s_profiles.active==sel)s_profiles.active=0;else if(s_profiles.active>sel)--s_profiles.active;
      if(sel>(int)s_profiles.count+1)sel=(int)s_profiles.count+1;
      r498_profiles_save();s_r499_session_auth=false;s_r499_session_auth_name[0]=0;strcpy(notice,"REMOVED FROM THIS CONSOLE");menu_latch_current();continue;
    }
    if(p&ZMENU_A){
      if(sel<(int)s_profiles.count){
        s_profiles.active=(uint8_t)sel;r498_profiles_save();R499ProfileEntry* e=r499_active_profile();char status[64]={0};bool ok=false;
        if(e&&e->token[0])ok=r499_profile_auth_dialog(false,e->name,NULL,status,sizeof(status));
        if(!ok&&e){char pass[R499_PASSWORD_MAX+1]={0};if(r499_keyboard("PROFILE LOGIN","PASSWORD",pass,sizeof(pass),R499_PASSWORD_MAX,true))ok=r499_profile_auth_dialog(false,e->name,pass,status,sizeof(status));}
        strncpy(notice,status[0]?status:(ok?"PROFILE READY":"LOGIN FAILED"),sizeof(notice)-1);notice[sizeof(notice)-1]=0;
      }else if(sel==action_create){
        if(s_profiles.count>=R498_PROFILE_MAX){strcpy(notice,"LOCAL PROFILE SLOTS FULL");}
        else {
          char user[R498_PROFILE_NAME_MAX+1]={0},pass[R499_PASSWORD_MAX+1]={0};
          if(r499_keyboard("CREATE ONLINE PROFILE","USERNAME",user,sizeof(user),R498_PROFILE_NAME_MAX,false)&&r499_keyboard("CREATE ONLINE PROFILE","PASSWORD",pass,sizeof(pass),R499_PASSWORD_MAX,true)){
            char clean[R498_PROFILE_NAME_MAX+1];r498_clean_name(user,clean,sizeof(clean));char status[64]={0};bool ok=strlen(clean)>=3&&strlen(pass)>=4&&r499_profile_auth_dialog(true,clean,pass,status,sizeof(status));
            strncpy(notice,status[0]?status:(ok?"PROFILE CREATED":"CREATE FAILED"),sizeof(notice)-1);notice[sizeof(notice)-1]=0;sel=s_profiles.active;
          }
        }
      }else if(sel==action_recover){
        if(s_profiles.count>=R498_PROFILE_MAX){strcpy(notice,"LOCAL PROFILE SLOTS FULL");}
        else {
          char user[R498_PROFILE_NAME_MAX+1]={0},pass[R499_PASSWORD_MAX+1]={0};
          if(r499_keyboard("SIGN IN / RECOVER","USERNAME",user,sizeof(user),R498_PROFILE_NAME_MAX,false)&&r499_keyboard("SIGN IN / RECOVER","PASSWORD",pass,sizeof(pass),R499_PASSWORD_MAX,true)){
            char clean[R498_PROFILE_NAME_MAX+1];r498_clean_name(user,clean,sizeof(clean));char status[64]={0};bool ok=strlen(clean)>=3&&strlen(pass)>=4&&r499_profile_auth_dialog(false,clean,pass,status,sizeof(status));
            strncpy(notice,status[0]?status:(ok?"PROFILE RECOVERED":"LOGIN FAILED"),sizeof(notice)-1);notice[sizeof(notice)-1]=0;if(ok)sel=s_profiles.active;
          }
        }
      }
      menu_latch_current();
    }
    menu_yield();
  }
}

bool Xbox_Netplay_EnsureProfile(void){
  r498_profiles_load();R499ProfileEntry* e=r499_active_profile();char status[64]={0};
  if(e&&e->token[0]&&s_r499_session_auth&&!strcmp(s_r499_session_auth_name,e->name)){
    Xbox_Log("ZONLINE R49.9.1 AUTH session-cache profile=%s\n",e->name);return true;
  }
  if(e&&e->token[0]&&r499_profile_auth_dialog(false,e->name,NULL,status,sizeof(status)))return true;
  Xbox_Netplay_ProfileMenu();e=r499_active_profile();
  if(e&&e->token[0]&&s_r499_session_auth&&!strcmp(s_r499_session_auth_name,e->name))return true;
  if(e&&e->token[0]&&r499_profile_auth_dialog(false,e->name,NULL,status,sizeof(status)))return true;
  menu_frame("ONLINE PROFILE","SIGNED-IN PROFILE REQUIRED",status[0]?status:"CREATE OR LOGIN FIRST","","","B BACK");Sleep(1100);return false;
}

static void r498_scores_defaults(void){memset(&s_scores,0,sizeof(s_scores));s_scores.magic=R498_SCORE_MAGIC;s_scores.schema=R498_SCORE_SCHEMA;s_scores_loaded=true;}
static void r498_scores_load(void){
  if(s_scores_loaded)return;r498_scores_defaults();char path[MAX_PATH];if(!r498_meta_path("scores-r499.dat",path,sizeof(path)))return;FILE* f=fopen(path,"rb");if(!f)return;R498ScoreStore t;bool ok=fread(&t,1,sizeof(t),f)==sizeof(t);fclose(f);if(ok&&t.magic==R498_SCORE_MAGIC&&t.schema==R498_SCORE_SCHEMA&&t.count<=R498_LOCAL_SCORES){s_scores=t;s_scores_loaded=true;}
}
static void r498_scores_save(void){r498_scores_load();char path[MAX_PATH];if(!r498_meta_path("scores-r499.dat",path,sizeof(path)))return;FILE* f=fopen(path,"wb");if(!f)return;fwrite(&s_scores,1,sizeof(s_scores),f);fclose(f);}

static int r498_ruleset_menu(const char* title,bool unused_leaderboard_row){
  (void)unused_leaderboard_row;int sel=0;const int count=5;menu_latch_current();
  for(;;){
    char a[64]="",b[64]="",c[64]="",d[64]="";if(sel<4){snprintf(a,sizeof(a),"> %s",r4995_ruleset_menu_name((uint8_t)sel));snprintf(b,sizeof(b),"  %s",r4995_ruleset_menu_name((uint8_t)((sel+1)%4)));snprintf(c,sizeof(c),"  %s",r4995_ruleset_menu_name((uint8_t)((sel+2)%4)));snprintf(d,sizeof(d),"MODE %d OF 4",sel+1);}else{strcpy(a,"> BACK");snprintf(d,sizeof(d),"PROFILE %s",Xbox_Netplay_ProfileName());}
    menu_frame(title,a,b,c,d,"UP/DOWN CHOOSE   A SELECT   B BACK");uint16_t p=menu_pressed();if(p&ZMENU_B)return -1;if(p&ZMENU_UP)sel=(sel+count-1)%count;if(p&ZMENU_DOWN)sel=(sel+1)%count;if(p&ZMENU_A){if(sel<4)return sel;return -1;}menu_yield();
  }
}

int Xbox_Netplay_SoloMenu(void){return r498_ruleset_menu("ONLINE SOLO",false);}

static void menu_ip_editor_frame(const char* title,const char* digits,int cursor,
                                 const char* prompt,const char* local,const char* footer){
  uint8_t* pixels=0;int pitch=0;menu_begin(&pixels,&pitch);
  if(pixels){
    menu_ui_text(pixels,pitch,44,43,title?title:"JOIN DIRECT",2,0x00FFFFFFu);

    const int x0=56,y0=120,scale=2,advance=6*scale;
    for(int i=0;digits&&digits[i];++i){
      char ch[2]={digits[i],0};
      int x=x0+i*advance;
      if(i==cursor){
        /* R47.4 used a '^' cursor, but '^' was not in the 5x7 font and was
         * therefore invisible. R48 highlights the actual editable digit. */
        menu_ui_rect(pixels,pitch,x-1,y0-2,advance,9*scale,0x00FFD35Au);
        menu_ui_text(pixels,pitch,x,y0,ch,scale,0x00101820u);
      }else{
        menu_ui_text(pixels,pitch,x,y0,ch,scale,0x0068FF68u);
      }
    }
    if(prompt)menu_ui_text(pixels,pitch,56,178,prompt,1,0x0068FF68u);
    menu_ui_text(pixels,pitch,56,214,"YELLOW BOX = CURRENT DIGIT",1,0x00FFD35Au);
    menu_ui_text(pixels,pitch,56,248,"LEFT/RIGHT MOVE   UP/DOWN CHANGE",1,0x0068FF68u);
    if(local)menu_ui_text(pixels,pitch,56,286,local,1,0x0068FF68u);
    if(footer)menu_ui_text(pixels,pitch,42,398,footer,1,0x00C8D4DCu);
  }
  menu_present();
}

static void ipv4_text(uint32_t host_ip,char* out,int cap){
  unsigned a=(host_ip>>24)&255u,b=(host_ip>>16)&255u,c=(host_ip>>8)&255u,d=host_ip&255u;
  snprintf(out,cap-1,"%u.%u.%u.%u",a,b,c,d);out[cap-1]=0;
}

static bool parse_ipv4(const char* s,uint32_t* host_ip){
  if(!s||!host_ip)return false;unsigned v[4]={0,0,0,0};int part=0,digits=0;
  for(const char* p=s;;++p){char ch=*p;if(ch>='0'&&ch<='9'){if(part>3)return false;v[part]=v[part]*10u+(unsigned)(ch-'0');if(v[part]>255u)return false;++digits;}
    else if(ch=='.'||ch==0){if(digits==0||part>3)return false;if(ch==0){if(part!=3)return false;break;}++part;digits=0;}
    else return false;}
  *host_ip=(v[0]<<24)|(v[1]<<16)|(v[2]<<8)|v[3];return true;
}

static bool resolve_local_ip(void){
  if(s_local_ip_cached && strcmp(s_local_ip,"UNAVAILABLE"))return true;
  strcpy(s_local_ip,"UNAVAILABLE");
  XNADDR xa;memset(&xa,0,sizeof(xa));
  DWORD flags=XNET_GET_XNADDR_PENDING;
  /* R49.9.3: XNet stays warm across online menus.  Poll for the first
   * DHCP/static address, then reuse it in RAM instead of doing this again. */
  for(int i=0;i<40;++i){
    flags=XNetGetTitleXnAddr(&xa);
    if(flags&(XNET_GET_XNADDR_DHCP|XNET_GET_XNADDR_STATIC))break;
    Sleep(50);
  }
  if(!(flags&(XNET_GET_XNADDR_DHCP|XNET_GET_XNADDR_STATIC))||!xa.ina.s_addr){
    Xbox_Log("ZNET R49.9.3 local IPv4 pending/unavailable flags=%08lX\n",(unsigned long)flags);
    return false;
  }
  const unsigned char* o=(const unsigned char*)&xa.ina;
  snprintf(s_local_ip,sizeof(s_local_ip)-1,"%u.%u.%u.%u",o[0],o[1],o[2],o[3]);
  s_local_ip[sizeof(s_local_ip)-1]=0;s_local_ip_cached=true;
  Xbox_Log("ZNET R49.9.3 local IPv4 cached %s udp/%d\n",s_local_ip,ZNP_PORT);
  return true;
}

static void local_ip_text(char* out,int cap){
  if(!out||cap<2)return;
  strncpy(out,s_local_ip,(size_t)cap-1);out[cap-1]=0;
}

/* R48 host-only WAN IPv4 discovery.
 * This mirrors the CURRENT MK64 OG Xbox public-IP path, including the
 * hardware-proven DNS-independent Google STUN endpoint FIRST. OG Xbox/XNet
 * hardware has shown XNetDnsLookup can remain WSAEINPROGRESS/10036 for DNS
 * names, so public-IP discovery must not depend on DNS succeeding.
 *
 * Order:
 *   1) 74.125.250.129:19302 (GOOGLE DIRECT, no DNS)
 *   2) stun.cloudflare.com:3478
 *   3) stun.cloudflare.com:53
 *   4) stun.l.google.com:19302
 *
 * The public address is UI-only and is never written to the diagnostic log. */
static bool stun_public_ipv4(const uint8_t* p,int n,const uint8_t tx[12],uint32_t* ip){
  if(!p||!ip||n<20)return false;
  if(p[0]!=0x01||p[1]!=0x01)return false;
  if(get32be(p+4)!=0x2112A442u)return false;
  if(memcmp(p+8,tx,12)!=0)return false;
  unsigned message=(unsigned)get16be(p+2);
  if(message+20u!=(unsigned)n||(message&3u))return false;
  for(unsigned off=20;off+4u<=(unsigned)n;){
    unsigned type=(unsigned)get16be(p+off);
    unsigned len=(unsigned)get16be(p+off+2);
    off+=4;
    if(len>(unsigned)n-off)return false;
    if(type==0x0020u&&len>=8u&&p[off+1]==0x01){
      *ip=get32be(p+off+4)^0x2112A442u;
      return *ip!=0u&&*ip!=0xffffffffu;
    }
    if(type==0x0001u&&len>=8u&&p[off+1]==0x01){
      *ip=get32be(p+off+4);
      return *ip!=0u&&*ip!=0xffffffffu;
    }
    off+=(len+3u)&~3u;
  }
  return false;
}

static void set_public_ip_text(uint32_t ip){
  snprintf(s_public_ip,sizeof(s_public_ip)-1,"%u.%u.%u.%u",
           (unsigned)((ip>>24)&255u),(unsigned)((ip>>16)&255u),
           (unsigned)((ip>>8)&255u),(unsigned)(ip&255u));
  s_public_ip[sizeof(s_public_ip)-1]=0;
}

static void public_ip_wait_screen(const char* label,const char* stage){
  char local[64];snprintf(local,sizeof(local),"LOCAL IP %s",s_local_ip);
  menu_frame("HOST DIRECT","FINDING PUBLIC IP",local,label,stage,"PLEASE WAIT");
}

static bool try_stun_addr(const sockaddr_in& stun,const char* label,uint32_t* public_ip){
  if(!public_ip)return false;
  uint8_t request[20]={0x00,0x01,0x00,0x00,0x21,0x12,0xA4,0x42,0,0,0,0,0,0,0,0,0,0,0,0};
  if(XNetRandom(request+8,12)!=0){
    Xbox_Log("ZNET R49 STUN transaction generation failed server=%s\n",label);
    return false;
  }

  DWORD begin=GetTickCount();DWORD last_send=0;unsigned sends=0;
  while(GetTickCount()-begin<4500u){
    DWORD now=GetTickCount();
    if(!last_send||now-last_send>=500u){
      int sr=sendto(s_sock,(const char*)request,sizeof(request),0,(const sockaddr*)&stun,sizeof(stun));
      ++sends;
      if(sr==SOCKET_ERROR&&sends<=3u)
        Xbox_Log("ZNET R49 STUN send failed server=%s try=%u wsa=%d\n",label,sends,WSAGetLastError());
      last_send=now;
    }

    uint8_t response[1024];sockaddr_in from;int from_len=sizeof(from);
    int n=recvfrom(s_sock,(char*)response,sizeof(response),0,(sockaddr*)&from,&from_len);
    if(n>0&&from.sin_addr.s_addr==stun.sin_addr.s_addr&&from.sin_port==stun.sin_port){
      uint32_t ip=0;
      if(stun_public_ipv4(response,n,request+8,&ip)){
        *public_ip=ip;
        Xbox_Log("ZNET R49 public IP discovery succeeded via %s\n",label);
        return true;
      }
    }
    public_ip_wait_screen(label,"UDP STUN");
    Sleep(10);
  }
  Xbox_Log("ZNET R49 STUN timed out server=%s sends=%u\n",label,sends);
  return false;
}

static bool try_stun_dns(const char* host,unsigned port,const char* label,uint32_t* public_ip){
  if(!host||!label||!public_ip)return false;
  XNDNS* dns=0;
  int rc=XNetDnsLookup(host,0,&dns);
  if(rc!=0||!dns){
    Xbox_Log("ZNET R49 DNS start failed server=%s rc=%d\n",label,rc);
    return false;
  }

  DWORD begin=GetTickCount();
  while(dns->iStatus==WSAEINPROGRESS&&GetTickCount()-begin<4500u){
    public_ip_wait_screen(label,"DNS LOOKUP");
    Sleep(10);
  }

  int status=dns->iStatus;
  int count=dns->cina;
  sockaddr_in stun;memset(&stun,0,sizeof(stun));
  stun.sin_family=AF_INET;stun.sin_port=htons((u_short)port);
  bool found=status==0&&count>0;
  if(found)stun.sin_addr=dns->aina[0];
  XNetDnsRelease(dns);
  if(!found){
    Xbox_Log("ZNET R49 DNS failed server=%s status=%d count=%d\n",label,status,count);
    return false;
  }
  return try_stun_addr(stun,label,public_ip);
}

static bool try_google_direct(uint32_t* public_ip){
  sockaddr_in stun;memset(&stun,0,sizeof(stun));
  stun.sin_family=AF_INET;
  stun.sin_port=htons(19302);
  stun.sin_addr.s_addr=htonl(0x4A7DFA81u); /* 74.125.250.129 */
  return try_stun_addr(stun,"GOOGLE DIRECT",public_ip);
}

static void discover_public_ip(void){
  DWORD now=GetTickCount();
  if(s_public_ip_cached && strcmp(s_public_ip,"UNAVAILABLE") &&
     (DWORD)(now-s_public_ip_cached_tick)<600000u){
    Xbox_Log("ZNET R49.9.3 public IP cache hit %s\n",s_public_ip);
    return;
  }
  strcpy(s_public_ip,"UNAVAILABLE");
  uint32_t ip=0;
  if(try_google_direct(&ip)||
     try_stun_dns("stun.cloudflare.com",3478,"CLOUDFLARE 3478",&ip)||
     try_stun_dns("stun.cloudflare.com",53,"CLOUDFLARE 53",&ip)||
     try_stun_dns("stun.l.google.com",19302,"GOOGLE 19302",&ip)){
    set_public_ip_text(ip);s_public_ip_cached=true;s_public_ip_cached_tick=GetTickCount();
    return;
  }
  strcpy(s_public_ip,"UNAVAILABLE");s_public_ip_cached=false;
  Xbox_Log("ZNET R49 all public-IP discovery fallbacks failed\n");
}

int Xbox_Netplay_DirectMenu(void){
  int sel=0;menu_latch_current();
  for(;;){
    char a[56],b[56],c[56];
    snprintf(a,sizeof(a),"%c HOST DIRECT",sel==0?'>':' ');
    snprintf(b,sizeof(b),"%c JOIN DIRECT",sel==1?'>':' ');
    snprintf(c,sizeof(c),"%c BACK",sel==2?'>':' ');
    menu_frame("DIRECT CONNECT",a,b,c,"","A SELECT   B BACK");
    uint16_t p=menu_pressed();if(p&ZMENU_B)return -1;
    if(p&ZMENU_UP)sel=(sel+2)%3;if(p&ZMENU_DOWN)sel=(sel+1)%3;
    if(p&ZMENU_A){if(sel==0)return 1;if(sel==1)return 2;return -1;}
    menu_yield();
  }
}

static int public_mode_menu(void){
  int sel=0;menu_latch_current();Xbox_Log("ZONLINE R49.9 PUBLIC selector opened profile=%s\n",Xbox_Netplay_ProfileName());public_diag_flush();
  for(;;){char a[56],b[56],c[56],d[56];snprintf(a,sizeof(a),"%c HOST PUBLIC ROOM",sel==0?'>':' ');snprintf(b,sizeof(b),"%c JOIN PUBLIC ROOM",sel==1?'>':' ');snprintf(c,sizeof(c),"%c WORLD CHAT",sel==2?'>':' ');snprintf(d,sizeof(d),"%c BACK",sel==3?'>':' ');menu_frame("PUBLIC ROOMS",a,b,c,d,"A SELECT   B BACK");uint16_t p=menu_pressed();if(p&ZMENU_B)return -1;if(p&ZMENU_UP)sel=(sel+3)%4;if(p&ZMENU_DOWN)sel=(sel+1)%4;if(p&ZMENU_A){if(sel==3)return -1;return sel;}menu_yield();}
}

static bool direct_ip_editor(const char* title,const char* prompt,sockaddr_in* out){
  if(!out)return false;char digits[16]="192.168.001.100";int cursor=0;menu_latch_current();
  for(;;){
    char local[64];snprintf(local,sizeof(local),"LOCAL IP %s",s_local_ip);
    menu_ip_editor_frame(title,digits,cursor,prompt,local,"UDP 6464   A CONNECT   B BACK");
    uint16_t p=menu_pressed();
    if(p&ZMENU_B)return false;
    if(p&ZMENU_LEFT){do{cursor=(cursor+14)%15;}while(digits[cursor]=='.');}
    if(p&ZMENU_RIGHT){do{cursor=(cursor+1)%15;}while(digits[cursor]=='.');}
    if(p&ZMENU_UP)digits[cursor]=(digits[cursor]=='9')?'0':(char)(digits[cursor]+1);
    if(p&ZMENU_DOWN)digits[cursor]=(digits[cursor]=='0')?'9':(char)(digits[cursor]-1);
    if(p&ZMENU_A){uint32_t ip=0;if(parse_ipv4(digits,&ip)){memset(out,0,sizeof(*out));out->sin_family=AF_INET;out->sin_port=htons(ZNP_PORT);out->sin_addr.s_addr=htonl(ip);return true;}}
    menu_yield();
  }
}

static bool peer_equal(const sockaddr_in& a, const sockaddr_in& b) {
  return a.sin_family == b.sin_family && a.sin_port == b.sin_port &&
         a.sin_addr.s_addr == b.sin_addr.s_addr;
}
static bool ensure_network_stack(void) {
  if(s_wsa)return true;
  XNetStartupParams xp;memset(&xp,0,sizeof(xp));
  xp.cfgSizeOfStruct=sizeof(xp);xp.cfgFlags=XNET_STARTUP_BYPASS_SECURITY;
  int xe=XNetStartup(&xp);
  if(xe){Xbox_Log("ZNET R49.9.3 XNetStartup failed err=%d\n",xe);return false;}
  WSADATA w;int we=WSAStartup(MAKEWORD(2,2),&w);
  if(we){Xbox_Log("ZNET R49.9.3 WSAStartup failed err=%d\n",we);XNetCleanup();return false;}
  s_wsa=true;resolve_local_ip();
  Xbox_Log("ZNET R49.9.3 network stack warm\n");
  return true;
}

static bool open_socket(bool public_socket) {
  if(!ensure_network_stack())return false;
  if(!s_local_ip_cached)resolve_local_ip();
  if(s_sock!=INVALID_SOCKET){closesocket(s_sock);s_sock=INVALID_SOCKET;}
  s_sock=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
  if(s_sock==INVALID_SOCKET){Xbox_Log("ZNET R49 socket failed wsa=%d\n",WSAGetLastError());return false;}
  BOOL raw=TRUE;
  int ro=setsockopt(s_sock,SOL_SOCKET,0x5801,(const char*)&raw,sizeof(raw));
  Xbox_Log("ZNET R49 raw-udp-5801=%s wsa=%d\n",ro==0?"OK":"optional-fail",ro==0?0:WSAGetLastError());
  int netbuf=64*1024;
  setsockopt(s_sock,SOL_SOCKET,SO_RCVBUF,(const char*)&netbuf,sizeof(netbuf));
  setsockopt(s_sock,SOL_SOCKET,SO_SNDBUF,(const char*)&netbuf,sizeof(netbuf));
  sockaddr_in a;memset(&a,0,sizeof(a));a.sin_family=AF_INET;a.sin_addr.s_addr=htonl(INADDR_ANY);a.sin_port=htons((u_short)ZNP_PORT);
  if(bind(s_sock,(sockaddr*)&a,sizeof(a))==SOCKET_ERROR){
    Xbox_Log("ZNET R49 bind udp/6464 failed wsa=%d\n",WSAGetLastError());
    closesocket(s_sock);s_sock=INVALID_SOCKET;return false;
  }
  if(public_socket){
    s_public_local_port=ZNP_PORT;
    Xbox_Log("ZNET R49 PUBLIC local endpoint %s:%u\n",s_local_ip,s_public_local_port);
    public_diag_flush();
  }
  u_long nonblock=1;
  if(ioctlsocket(s_sock,FIONBIO,&nonblock)==SOCKET_ERROR){
    Xbox_Log("ZNET R49 nonblock failed wsa=%d\n",WSAGetLastError());
    closesocket(s_sock);s_sock=INVALID_SOCKET;return false;
  }
  return true;
}

static void public_reset(void){
  s_public_mode=false;s_public_host=false;s_public_have_self=false;s_public_have_intro=false;s_public_direct=false;
  s_public_room[0]=s_public_token[0]=s_public_cookie[0]=s_public_peer_cookie[0]=0;
  memset(&s_public_peer,0,sizeof(s_public_peer));
  s_public_last_keep=s_public_last_heartbeat=s_public_last_punch=s_public_last_direct_rx=0;
  s_public_relay_tx=s_public_relay_rx=0;s_public_direct_valid=0;
  s_public_local_direct_ok=false;s_public_peer_direct_ok=false;s_public_local_port=0;s_public_lan_candidate=false;
}

static sockaddr_in public_directory_addr(void){sockaddr_in a;memset(&a,0,sizeof(a));a.sin_family=AF_INET;a.sin_port=htons((u_short)ZDIR_PORT);a.sin_addr.s_addr=inet_addr(ZDIR_IP);return a;}
static bool public_from_server(const sockaddr_in& a){sockaddr_in d=public_directory_addr();return peer_equal(a,d);}
static bool public_send_text(const char* text){if(!text||s_sock==INVALID_SOCKET)return false;sockaddr_in d=public_directory_addr();int n=(int)strlen(text);return sendto(s_sock,text,n,0,(const sockaddr*)&d,sizeof(d))==n;}


struct R498BoardRow { uint32_t frames; uint8_t players; char name[25]; char date[11]; };

// R49.9.3: leaderboard navigation never blocks on the network.
struct R499BoardCacheEntry {
  uint8_t valid; /* 0 unknown, 1 valid, 2 last request failed */
  uint8_t count;
  uint16_t reserved;
  DWORD stamp;
  R498BoardRow rows[5];
};
static R499BoardCacheEntry s_board_cache[2][4][56];

struct R499BoardAsync {
  uint8_t active,scope,ruleset,attempts;
  uint16_t level,reserved;
  uint32_t nonce;
  DWORD due_tick,sent_tick;
};
static R499BoardAsync s_board_async;
static uint32_t s_board_nonce=1;

struct R499ScoreUploadAsync {
  uint8_t active,attempts;
  uint16_t index;
  DWORD sent_tick;
};
static R499ScoreUploadAsync s_score_upload_async;

static void r498_time_text(uint32_t frames,char* out,int cap){
  const uint64_t ms=((uint64_t)frames*1000ull+30ull)/60ull;
  const uint32_t mins=(uint32_t)(ms/60000ull),secs=(uint32_t)((ms/1000ull)%60ull),milli=(uint32_t)(ms%1000ull);
  snprintf(out,(size_t)cap,"%02lu:%02lu.%03lu",(unsigned long)mins,(unsigned long)secs,(unsigned long)milli);if(cap>0)out[cap-1]=0;
}

static int r499_recv_prefix(SOCKET sock,const char* prefix,char* out,int cap){
  if(!out||cap<2||sock==INVALID_SOCKET)return -1;const int plen=(int)strlen(prefix);
  for(int tries=0;tries<24;++tries){uint8_t raw[768];sockaddr_in from;int flen=sizeof(from);int n=recvfrom(sock,(char*)raw,sizeof(raw)-1,0,(sockaddr*)&from,&flen);if(n==SOCKET_ERROR){int e=WSAGetLastError();return e==WSAEWOULDBLOCK?0:-1;}raw[n]=0;if(public_from_server(from)&&n>=plen&&!strncmp((const char*)raw,prefix,(size_t)plen)){int c=n<cap-1?n:cap-1;memcpy(out,raw,c);out[c]=0;return 1;}}
  return 0;
}

static bool r499_send_on(SOCKET sock,const char* text){if(sock==INVALID_SOCKET||!text)return false;sockaddr_in d=public_directory_addr();int n=(int)strlen(text);return sendto(sock,text,n,0,(const sockaddr*)&d,sizeof(d))==n;}

static SOCKET r499_aux_socket(void){
  if(!ensure_network_stack())return INVALID_SOCKET;
  SOCKET q=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);if(q==INVALID_SOCKET)return q;
  sockaddr_in a;memset(&a,0,sizeof(a));a.sin_family=AF_INET;a.sin_addr.s_addr=htonl(INADDR_ANY);a.sin_port=0;
  if(bind(q,(sockaddr*)&a,sizeof(a))==SOCKET_ERROR){closesocket(q);return INVALID_SOCKET;}
  u_long nb=1;ioctlsocket(q,FIONBIO,&nb);return q;
}

static void r499_password_proof(const char* user,const char* password,char out[33]){
  // The plaintext password never leaves the console.  This deliberately uses
  // a CPU-cheap iterated verifier suitable for a Pentium-III Xbox; the private
  // service applies PBKDF2 again before persisting the verifier.
  uint64_t a=0x9e3779b97f4a7c15ull,b=0xd6e8feb86659fd93ull;
  char tmp[96];snprintf(tmp,sizeof(tmp),"ZAMN-R499:%s:%s",user?user:"",password?password:"");
  a=r499_fnv64((const uint8_t*)tmp,(int)strlen(tmp),a);b=r499_fnv64((const uint8_t*)tmp,(int)strlen(tmp),b^0xa5a5a5a5a5a5a5a5ull);
  for(unsigned i=0;i<4096u;++i){uint8_t mix[20];memcpy(mix,&a,8);memcpy(mix+8,&b,8);mix[16]=(uint8_t)i;mix[17]=(uint8_t)(i>>8);mix[18]=(uint8_t)(i>>16);mix[19]=(uint8_t)(i>>24);a=r499_fnv64(mix,20,a^b);b=r499_fnv64(mix,20,b^a^0x517cc1b727220a95ull);}
  snprintf(out,33,"%08lX%08lX%08lX%08lX",(unsigned long)(a>>32),(unsigned long)a,(unsigned long)(b>>32),(unsigned long)b);out[32]=0;
}

static bool r499_profile_auth_dialog(bool create_account,const char* user,const char* password,char* status,int status_cap){
  if(status&&status_cap>0)status[0]=0;if(!user||!user[0])return false;
  R499BlockingAudioMute audio_mute;
  if(!ensure_network_stack()){if(status)snprintf(status,(size_t)status_cap,"SERVER NETWORK FAILED");return false;}
  char console[17]={0};if(!r499_console_id_text(console)){if(status)snprintf(status,(size_t)status_cap,"CONSOLE ID UNAVAILABLE");return false;}
  SOCKET q=r499_aux_socket();if(q==INVALID_SOCKET){if(status)snprintf(status,(size_t)status_cap,"SERVER SOCKET FAILED");return false;}
  R499ProfileEntry* active=r499_active_profile();const char* token=(active&&active->name[0]&&!strcmp(active->name,user))?active->token:"";
  char msg[256],resp[256];
  if(!create_account&&(!password||!password[0])&&token&&token[0])
    snprintf(msg,sizeof(msg),"ZAUTH1|TOKEN|%08lX|%08lX%08lX|%s|%s|%s",(unsigned long)ZNP_BUILD,(unsigned long)(s_rom_hash>>32),(unsigned long)s_rom_hash,user,token,console);
  else {char proof[33];r499_password_proof(user,password?password:"",proof);snprintf(msg,sizeof(msg),"ZAUTH1|%s|%08lX|%08lX%08lX|%s|%s|%s",create_account?"CREATE":"LOGIN",(unsigned long)ZNP_BUILD,(unsigned long)(s_rom_hash>>32),(unsigned long)s_rom_hash,user,proof,console);}
  msg[sizeof(msg)-1]=0;DWORD begin=GetTickCount(),last=0;bool ok=false;
  while(GetTickCount()-begin<2500u){
    DWORD now=GetTickCount();if(!last||now-last>=350u){r499_send_on(q,msg);last=now;}
    int rr=r499_recv_prefix(q,"ZAUTH1|",resp,sizeof(resp));if(rr<0)break;
    if(rr>0){
      char name[16]={0},tok[40]={0},warn[64]={0};unsigned wc=0;
      if(sscanf(resp,"ZAUTH1|OK|%15[^|]|%39[^|]|%u|%63[^\n]",name,tok,&wc,warn)>=3){
        ok=r499_store_profile_auth(name,tok);
        if(status){if(wc)snprintf(status,(size_t)status_cap,"WARNING %u: %.38s",wc,warn[0]?warn:"CHECK ACCOUNT");else snprintf(status,(size_t)status_cap,"PROFILE READY");}
        Xbox_Log("ZONLINE R49.9.3 AUTH ok profile=%s warnings=%u console=%s service-socket=1\n",name,wc,console);Xbox_LogFlush();break;
      }
      if(!strncmp(resp,"ZAUTH1|ERR|",11)){
        const char* e=resp+11;if(status){strncpy(status,e,(size_t)status_cap-1);status[status_cap-1]=0;}
        Xbox_Log("ZONLINE R49.9.3 AUTH reject profile=%s reason=%s console=%s\n",user,e,console);Xbox_LogFlush();break;
      }
    }
    Sleep(4);
  }
  closesocket(q);return ok;
}


static bool r499_same_score_key(const R498LocalScore& r,uint8_t ruleset,uint16_t level,uint8_t scope,const char* name,const char* peer){
  return r.ruleset==(ruleset&3u)&&r.level==level&&r.scope==scope&&!strcmp(r.name,name?name:"")&&!strcmp(r.peer,peer?peer:"");
}

static void r498_score_add_local(uint8_t ruleset,uint16_t level,uint32_t frames,uint8_t players,uint8_t scope,const char* name,const char* peer){
  if(level<1||level>56||frames==0||!name||!name[0])return;r498_scores_load();
  for(uint32_t i=0;i<s_scores.count;++i){R498LocalScore& r=s_scores.rows[i];if(r499_same_score_key(r,ruleset,level,scope,name,peer)){if(frames<r.frames){Xbox_Log("ZONLINE R49.9 SCORE local-best scope=%s profile=%s peer=%s level=%u mode=%u old=%lu new=%lu\n",scope==R499_SCOPE_SOLO?"SOLO ONLINE":"CO-OP ONLINE",name,peer?peer:"",(unsigned)level,(unsigned)ruleset,(unsigned long)r.frames,(unsigned long)frames);r.frames=frames;r.players=players;r.sent=0;r498_scores_save();}else Xbox_Log("ZONLINE R49.9 SCORE local-ignore-not-better profile=%s level=%u mode=%u frames=%lu best=%lu\n",name,(unsigned)level,(unsigned)ruleset,(unsigned long)frames,(unsigned long)r.frames);return;}}
  if(s_scores.count>=R498_LOCAL_SCORES){memmove(&s_scores.rows[0],&s_scores.rows[1],sizeof(s_scores.rows[0])*(R498_LOCAL_SCORES-1));s_scores.count=R498_LOCAL_SCORES-1;}
  R498LocalScore& r=s_scores.rows[s_scores.count++];memset(&r,0,sizeof(r));r.ruleset=(uint8_t)(ruleset&3u);r.level=level;r.frames=frames;r.players=players;r.sent=0;r.scope=scope;strncpy(r.name,name,R498_PROFILE_NAME_MAX);r.name[R498_PROFILE_NAME_MAX]=0;if(peer){strncpy(r.peer,peer,R498_PROFILE_NAME_MAX);r.peer[R498_PROFILE_NAME_MAX]=0;}r498_scores_save();
  Xbox_Log("ZONLINE R49.9 SCORE queued scope=%s profile=%s peer=%s level=%u mode=%u frames=%lu players=%u\n",scope==R499_SCOPE_SOLO?"SOLO":"ONLINE",r.name,r.peer,(unsigned)level,(unsigned)ruleset,(unsigned long)frames,(unsigned)players);Xbox_LogFlush();
}


void Xbox_Netplay_RecordLevelTime(uint8_t ruleset,uint16_t level,uint32_t frames,uint8_t players,bool solo){
  const char* me=Xbox_Netplay_ProfileName();const char* peer=(!solo&&players>1)?Xbox_Netplay_PeerProfileName():"";const bool join_mirror=!solo&&s_stats.active&&s_core.local_slot!=0;
  Xbox_Log("ZONLINE R49.9 LEVEL complete scope=%s profile=%s peer=%s level=%u mode=%u frames=%lu time60fps=%lu.%03lus\n",solo?"SOLO":"ONLINE",me,peer,(unsigned)level,(unsigned)ruleset,(unsigned long)frames,(unsigned long)(frames/60u),(unsigned long)(((uint64_t)(frames%60u)*1000ull+30ull)/60ull));Xbox_LogFlush();
  if(join_mirror)return;r498_score_add_local(ruleset,level,frames,players,solo?R499_SCOPE_SOLO:R499_SCOPE_ONLINE,me,peer);
}


static int r498_local_board(uint8_t ruleset,uint16_t level,uint8_t scope,R498BoardRow out[5]){
  r498_scores_load();R498BoardRow all[R498_LOCAL_SCORES];int n=0;for(uint32_t i=0;i<s_scores.count&&n<R498_LOCAL_SCORES;++i){const R498LocalScore& r=s_scores.rows[i];if(r.ruleset==(ruleset&3u)&&r.level==level&&r.scope==scope){memset(&all[n],0,sizeof(all[n]));all[n].frames=r.frames;all[n].players=r.players;strcpy(all[n].date,r.sent?"SYNCED":"PENDING");if(scope==R499_SCOPE_ONLINE&&r.peer[0])snprintf(all[n].name,sizeof(all[n].name),"%.11s+%.11s",r.name,r.peer);else{strncpy(all[n].name,r.name,24);all[n].name[24]=0;}++n;}}
  for(int i=0;i<n;++i)for(int j=i+1;j<n;++j)if(all[j].frames<all[i].frames){R498BoardRow t=all[i];all[i]=all[j];all[j]=t;}int take=n<5?n:5;for(int i=0;i<take;++i)out[i]=all[i];return take;
}

static R499BoardCacheEntry* r499_board_cache_entry(uint8_t scope,uint8_t ruleset,uint16_t level){
  if(scope>1u||ruleset>3u||level<1u||level>56u)return NULL;
  return &s_board_cache[scope][ruleset][level-1u];
}
static int r499_pending_score_count(void){
  r498_scores_load();int n=0;for(uint32_t i=0;i<s_scores.count;++i)if(!s_scores.rows[i].sent)++n;return n;
}
static void r499_board_cache_invalidate(uint8_t scope,uint8_t ruleset,uint16_t level){
  R499BoardCacheEntry* c=r499_board_cache_entry(scope,ruleset,level);if(c){c->valid=0;c->stamp=0;}
}
static bool r499_parse_board_pack(char* resp,uint32_t want_nonce,R498BoardRow out[5],int* out_count){
  if(!resp||!out_count)return false;
  char* tok=strtok(resp,"|");if(!tok||strcmp(tok,"ZLB2"))return false;
  tok=strtok(NULL,"|");if(!tok)return false;const bool dated=!strcmp(tok,"PACKD");if(!dated&&strcmp(tok,"PACK"))return false;
  tok=strtok(NULL,"|");if(!tok||(uint32_t)strtoul(tok,NULL,10)!=want_nonce)return false;
  tok=strtok(NULL,"|");if(!tok)return false;int count=atoi(tok);if(count<0)count=0;if(count>5)count=5;
  memset(out,0,sizeof(R498BoardRow)*5);
  for(int i=0;i<count;++i){
    char* f=strtok(NULL,"|");char* pl=strtok(NULL,"|");char* nm=strtok(NULL,"|");char* dt=dated?strtok(NULL,"|"):NULL;
    if(!f||!pl||!nm||(dated&&!dt))return false;
    out[i].frames=(uint32_t)strtoul(f,NULL,10);out[i].players=(uint8_t)atoi(pl);
    strncpy(out[i].name,nm,24);out[i].name[24]=0;
    if(dt){strncpy(out[i].date,dt,10);out[i].date[10]=0;}else strcpy(out[i].date,"--");
  }
  *out_count=count;return true;
}
static void r499_board_async_start(uint8_t scope,uint8_t ruleset,uint16_t level,bool immediate){
  if(scope>1u||ruleset>3u||level<1u||level>56u)return;
  memset(&s_board_async,0,sizeof(s_board_async));s_board_async.active=1;s_board_async.scope=scope;
  s_board_async.ruleset=(uint8_t)(ruleset&3u);s_board_async.level=level;
  if(++s_board_nonce==0)s_board_nonce=1;s_board_async.nonce=s_board_nonce;
  s_board_async.due_tick=GetTickCount()+(immediate?0u:180u);
}
static void r499_board_async_tick(SOCKET sock){
  if(!s_board_async.active||sock==INVALID_SOCKET)return;
  DWORD now=GetTickCount();if((LONG)(now-s_board_async.due_tick)<0)return;
  if(!s_board_async.sent_tick||(DWORD)(now-s_board_async.sent_tick)>=350u){
    if(s_board_async.attempts>=3u){
      R499BoardCacheEntry* c=r499_board_cache_entry(s_board_async.scope,s_board_async.ruleset,s_board_async.level);
      if(c&&c->valid!=1u){c->valid=2u;c->stamp=now;}
      Xbox_Log("ZONLINE R49.9.3 BOARD async-timeout scope=%s mode=%u level=%u attempts=%u\n",
        s_board_async.scope==R499_SCOPE_SOLO?"SOLO":"CO-OP",(unsigned)s_board_async.ruleset,
        (unsigned)s_board_async.level,(unsigned)s_board_async.attempts);
      s_board_async.active=0;return;
    }
    const char* token=r499_profile_token();if(!token||!token[0]){s_board_async.active=0;return;}
    const char* op=s_board_async.attempts<2u?"LISTD":"LISTP";
    char q[224];snprintf(q,sizeof(q),"ZLB2|%s|%08lX|%08lX%08lX|%s|%u|%u|%u|%lu",op,
      (unsigned long)ZNP_BUILD,(unsigned long)(s_rom_hash>>32),(unsigned long)s_rom_hash,token,
      (unsigned)s_board_async.scope,(unsigned)s_board_async.ruleset,(unsigned)s_board_async.level,
      (unsigned long)s_board_async.nonce);
    r499_send_on(sock,q);s_board_async.sent_tick=now;++s_board_async.attempts;
  }
  for(int n=0;n<4;++n){
    char resp[768];int rr=r499_recv_prefix(sock,"ZLB2|",resp,sizeof(resp));if(rr<=0)break;
    if(!strncmp(resp,"ZLB2|PACKD|",11)||!strncmp(resp,"ZLB2|PACK|",10)){
      R498BoardRow rows[5];int count=0;
      if(r499_parse_board_pack(resp,s_board_async.nonce,rows,&count)){
        R499BoardCacheEntry* c=r499_board_cache_entry(s_board_async.scope,s_board_async.ruleset,s_board_async.level);
        if(c){memset(c,0,sizeof(*c));c->valid=1u;c->count=(uint8_t)count;c->stamp=GetTickCount();for(int i=0;i<count;++i)c->rows[i]=rows[i];}
        Xbox_Log("ZONLINE R49.9.3 BOARD async-ready scope=%s mode=%u level=%u rows=%d attempts=%u\n",
          s_board_async.scope==R499_SCOPE_SOLO?"SOLO":"CO-OP",(unsigned)s_board_async.ruleset,
          (unsigned)s_board_async.level,count,(unsigned)s_board_async.attempts);
        s_board_async.active=0;return;
      }
    }else if(!strncmp(resp,"ZLB2|ERR|",10)){
      R499BoardCacheEntry* c=r499_board_cache_entry(s_board_async.scope,s_board_async.ruleset,s_board_async.level);
      if(c&&c->valid!=1u){c->valid=2u;c->stamp=GetTickCount();}
      Xbox_Log("ZONLINE R49.9.3 BOARD reject %s\n",resp+10);s_board_async.active=0;return;
    }
  }
}
static void r499_score_upload_kick(void){
  if(s_score_upload_async.active)return;r498_scores_load();
  for(uint32_t i=0;i<s_scores.count;++i)if(!s_scores.rows[i].sent){
    memset(&s_score_upload_async,0,sizeof(s_score_upload_async));s_score_upload_async.active=1;
    s_score_upload_async.index=(uint16_t)i;return;
  }
}
static void r499_score_upload_tick(SOCKET sock){
  if(!s_score_upload_async.active||sock==INVALID_SOCKET)return;
  r498_scores_load();if(s_score_upload_async.index>=s_scores.count){s_score_upload_async.active=0;return;}
  R498LocalScore& r=s_scores.rows[s_score_upload_async.index];
  if(r.sent){s_score_upload_async.active=0;r499_score_upload_kick();return;}
  DWORD now=GetTickCount();
  if(!s_score_upload_async.sent_tick||(DWORD)(now-s_score_upload_async.sent_tick)>=350u){
    if(s_score_upload_async.attempts>=3u){
      Xbox_Log("ZONLINE R49.9.3 SCORE async-timeout profile=%s level=%u frames=%lu\n",r.name,(unsigned)r.level,(unsigned long)r.frames);
      s_score_upload_async.active=0;return;
    }
    const char* token=r499_profile_token();if(!token||!token[0]){s_score_upload_async.active=0;return;}
    char msg[256];snprintf(msg,sizeof(msg)-1,"ZLB2|SUBMIT|%08lX|%08lX%08lX|%s|%u|%u|%u|%lu|%u|%s",
      (unsigned long)ZNP_BUILD,(unsigned long)(s_rom_hash>>32),(unsigned long)s_rom_hash,token,
      (unsigned)r.scope,(unsigned)r.ruleset,(unsigned)r.level,(unsigned long)r.frames,
      (unsigned)r.players,r.peer[0]?r.peer:"-");msg[sizeof(msg)-1]=0;
    r499_send_on(sock,msg);s_score_upload_async.sent_tick=now;++s_score_upload_async.attempts;
  }
  for(int n=0;n<3;++n){
    char resp[192];int rr=r499_recv_prefix(sock,"ZLB2|",resp,sizeof(resp));if(rr<=0)break;
    if(!strncmp(resp,"ZLB2|OK|BEST",12)||!strncmp(resp,"ZLB2|OK|UNCHANGED",17)){
      r.sent=1;r498_scores_save();r499_board_cache_invalidate(r.scope,r.ruleset,r.level);
      Xbox_Log("ZONLINE R49.9.3 SCORE async-ack profile=%s level=%u frames=%lu result=%s\n",
        r.name,(unsigned)r.level,(unsigned long)r.frames,strstr(resp,"BEST")?"BEST":"UNCHANGED");
      s_score_upload_async.active=0;r499_score_upload_kick();return;
    }
    if(!strncmp(resp,"ZLB2|ERR|",10)){
      Xbox_Log("ZONLINE R49.9.3 SCORE async-reject profile=%s level=%u reason=%s\n",r.name,(unsigned)r.level,resp+10);
      s_score_upload_async.active=0;return;
    }
  }
}



static void r4995_compact_date(const char* src,char out[9]) {
  // Server dates are YYYY-MM-DD UTC. Keep the leaderboard compact as MM/DD/YY.
  if(src && strlen(src)>=10 && src[4]=='-' && src[7]=='-') {
    out[0]=src[5];out[1]=src[6];out[2]='/';out[3]=src[8];out[4]=src[9];
    out[5]='/';out[6]=src[2];out[7]=src[3];out[8]=0;
  } else { strcpy(out,"--/--/--"); }
}

static void r498_board_frame(uint8_t ruleset,uint16_t level,uint8_t scope,const R498BoardRow* rows,int count,int source){
  uint8_t* pixels=0;int pitch=0;menu_begin(&pixels,&pitch);
  if(pixels){
    // R49.9.4: fixed rows instead of a wrapping yellow header. This prevents
    // the mode/scope text from crossing the green status text on 480i output.
    menu_ui_text(pixels,pitch,36,34,"LEADERBOARDS",2,0x00FFFFFFu);
    menu_ui_text(pixels,pitch,38,72,scope==R499_SCOPE_SOLO?"SOLO ONLINE LEADERBOARDS":"CO-OP ONLINE LEADERBOARDS",1,0x00FFD35Au);
    char leveltext[24];snprintf(leveltext,sizeof(leveltext),"LEVEL %02u",(unsigned)level);
    menu_ui_text(pixels,pitch,38,96,leveltext,1,0x0068FF68u);
    menu_ui_text(pixels,pitch,154,96,r498_ruleset_name(ruleset),1,0x0068FF68u);
    const char* src=source==1?"GLOBAL TOP 5":source==2?"LOCAL BEST - SYNCING":source==3?"REFRESHING IN BACKGROUND":"LOCAL BEST - SERVER OFFLINE";
    menu_ui_text(pixels,pitch,38,120,src,1,source==1?0x0068FF68u:0x00FFD35Au);
    if(count<=0)menu_ui_text(pixels,pitch,54,176,source==3?"CHECKING SERVER...":"NO TIMES YET",2,0x0068FF68u);
    for(int i=0;i<count&&i<5;++i){
      char t[24],date[9],line[80];r498_time_text(rows[i].frames,t,sizeof(t));
      r4995_compact_date(rows[i].date,date);
      // Scope already tells players whether this is Solo or Co-op, so keep each
      // record to one clean row: rank, profile/team, time, date.
      snprintf(line,sizeof(line),"%d %-14.14s %s  %s",i+1,rows[i].name,t,date);
      const int y=154+i*38;
      menu_ui_text(pixels,pitch,42,y,line,1,i==0?0x00FFFFFFu:0x0068FF68u);
    }
    menu_ui_text(pixels,pitch,34,397,"LEFT/RIGHT LEVEL  UP/DOWN MODE  A REFRESH  B BACK",1,0x00C8D4DCu);
  }
  menu_present();
}

static int r499_leaderboard_scope_menu(void){int sel=0;menu_latch_current();for(;;){char a[64],b[64],c[64];snprintf(a,sizeof(a),"%c SOLO ONLINE LEADERBOARDS",sel==0?'>':' ');snprintf(b,sizeof(b),"%c CO-OP ONLINE LEADERBOARDS",sel==1?'>':' ');snprintf(c,sizeof(c),"%c BACK",sel==2?'>':' ');menu_frame("LEADERBOARDS",a,b,c,Xbox_Netplay_ProfileName(),"A SELECT   B BACK");uint16_t p=menu_pressed();if(p&ZMENU_B)return -1;if(p&ZMENU_UP)sel=(sel+2)%3;if(p&ZMENU_DOWN)sel=(sel+1)%3;if(p&ZMENU_A){if(sel<2)return sel;return -1;}menu_yield();}}

void Xbox_Netplay_LeaderboardMenu(void){
  if(!Xbox_Netplay_EnsureProfile())return;
  SOCKET board_sock=r499_aux_socket();SOCKET upload_sock=r499_aux_socket();
  const bool network=board_sock!=INVALID_SOCKET;
  if(upload_sock!=INVALID_SOCKET)r499_score_upload_kick();
  memset(&s_board_async,0,sizeof(s_board_async));
  for(;;){
    int sc=r499_leaderboard_scope_menu();if(sc<0)break;
    uint8_t scope=(uint8_t)sc,mode=s_ruleset;uint16_t level=1;
    R499BoardCacheEntry* c=r499_board_cache_entry(scope,mode,level);
    if(network&&(!c||c->valid!=1u||(DWORD)(GetTickCount()-c->stamp)>30000u))r499_board_async_start(scope,mode,level,true);
    menu_latch_current();
    for(;;){
      if(network)r499_board_async_tick(board_sock);
      if(upload_sock!=INVALID_SOCKET)r499_score_upload_tick(upload_sock);
      R498BoardRow rows[5];memset(rows,0,sizeof(rows));int count=0,source=0;
      c=r499_board_cache_entry(scope,mode,level);
      if(c&&c->valid==1u){
        count=c->count;for(int i=0;i<count&&i<5;++i)rows[i]=c->rows[i];source=1;
      }else{
        count=r498_local_board(mode,level,scope,rows);
        if(network&&c&&c->valid==0u)source=3;
        else if(count>0&&r499_pending_score_count()>0)source=2;
        else source=0;
      }
      r498_board_frame(mode,level,scope,rows,count,source);
      uint16_t p=menu_pressed();if(p&ZMENU_B){s_board_async.active=0;break;}
      bool changed=false;
      if(p&ZMENU_LEFT){level=level<=1?56:(uint16_t)(level-1);changed=true;}
      if(p&ZMENU_RIGHT){level=level>=56?1:(uint16_t)(level+1);changed=true;}
      if(p&ZMENU_UP){mode=(uint8_t)((mode+3)&3);changed=true;}
      if(p&ZMENU_DOWN){mode=(uint8_t)((mode+1)&3);changed=true;}
      if(changed&&network){
        c=r499_board_cache_entry(scope,mode,level);
        if(!c||c->valid!=1u||(DWORD)(GetTickCount()-c->stamp)>30000u)r499_board_async_start(scope,mode,level,false);
        else s_board_async.active=0;
      }
      if(p&ZMENU_A){
        if(upload_sock!=INVALID_SOCKET)r499_score_upload_kick();
        r499_board_cache_invalidate(scope,mode,level);
        if(network)r499_board_async_start(scope,mode,level,true);
      }
      c=r499_board_cache_entry(scope,mode,level);
      if(network&&c&&c->valid==0u&&!s_board_async.active)r499_board_async_start(scope,mode,level,false);
      menu_yield();
    }
  }
  if(board_sock!=INVALID_SOCKET)closesocket(board_sock);
  if(upload_sock!=INVALID_SOCKET)closesocket(upload_sock);
  memset(&s_board_async,0,sizeof(s_board_async));memset(&s_score_upload_async,0,sizeof(s_score_upload_async));
  menu_latch_current();
}

static int r499_chat_fetch(SOCKET q,bool lobby,R499ChatRow out[6]){
  R499BlockingAudioMute audio_mute;
  const char* token=r499_profile_token();if(!token||!token[0])return -1;char msg[160],resp[256];snprintf(msg,sizeof(msg),"ZCHAT1|LIST|%s|%s|%s",token,lobby?"LOBBY":"WORLD",lobby?s_public_room:"-");DWORD begin=GetTickCount(),last=0;int n=0;bool begun=false;while(GetTickCount()-begin<2200u){DWORD now=GetTickCount();if(!last||now-last>=350u){r499_send_on(q,msg);last=now;}int rr=r499_recv_prefix(q,"ZCHAT1|",resp,sizeof(resp));if(rr<0)return -1;if(rr>0){if(!strncmp(resp,"ZCHAT1|BEGIN|",13)){n=0;begun=true;continue;}if(!strcmp(resp,"ZCHAT1|END"))return n;if(begun&&!strncmp(resp,"ZCHAT1|ROW|",11)&&n<6){unsigned long seq=0;char name[16]={0},text[64]={0};if(sscanf(resp,"ZCHAT1|ROW|%lu|%15[^|]|%63[^\n]",&seq,name,text)==3){out[n].seq=(uint32_t)seq;strncpy(out[n].name,name,R498_PROFILE_NAME_MAX);out[n].name[R498_PROFILE_NAME_MAX]=0;strncpy(out[n].text,text,48);out[n].text[48]=0;++n;}}if(!strncmp(resp,"ZCHAT1|ERR|",11))return -1;}Sleep(4);}return -1;
}

static bool r499_chat_send(SOCKET q,bool lobby,const char* text){
  R499BlockingAudioMute audio_mute;
  const char* token=r499_profile_token();if(!token||!token[0]||!text||!text[0])return false;char msg[256],resp[128];snprintf(msg,sizeof(msg),"ZCHAT1|SEND|%s|%s|%s|%s",token,lobby?"LOBBY":"WORLD",lobby?s_public_room:"-",text);DWORD begin=GetTickCount(),last=0;while(GetTickCount()-begin<2200u){DWORD now=GetTickCount();if(!last||now-last>=300u){r499_send_on(q,msg);last=now;}int rr=r499_recv_prefix(q,"ZCHAT1|",resp,sizeof(resp));if(rr<0)return false;if(rr>0){if(!strncmp(resp,"ZCHAT1|OK|SEND",14)){Xbox_Log("ZONLINE R49.9 CHAT send channel=%s profile=%s room=%s\n",lobby?"LOBBY":"WORLD",Xbox_Netplay_ProfileName(),lobby?s_public_room:"-");Xbox_LogFlush();return true;}if(!strncmp(resp,"ZCHAT1|ERR|",11)){Xbox_Log("ZONLINE R49.9 CHAT reject channel=%s reason=%s\n",lobby?"LOBBY":"WORLD",resp+11);Xbox_LogFlush();return false;}}Sleep(4);}return false;
}

static void r499_chat_menu(bool lobby_default){
  SOCKET q=r499_aux_socket();if(q==INVALID_SOCKET){menu_frame("CHAT","NETWORK SOCKET FAILED","","","","B BACK");Sleep(800);return;}bool lobby=lobby_default&&s_public_room[0];bool refresh=true;R499ChatRow rows[6];int count=0;menu_latch_current();for(;;){if(refresh){memset(rows,0,sizeof(rows));count=r499_chat_fetch(q,lobby,rows);refresh=false;}uint8_t* pixels=0;int pitch=0;menu_begin(&pixels,&pitch);if(pixels){menu_ui_text(pixels,pitch,38,36,lobby?"LOBBY CHAT":"WORLD CHAT",2,0x00FFFFFFu);char sub[72];snprintf(sub,sizeof(sub),"PROFILE %s%s%s",Xbox_Netplay_ProfileName(),lobby?"  ROOM ":"",lobby?s_public_room:"");menu_ui_text(pixels,pitch,40,82,sub,1,0x00FFD35Au);if(count<=0)menu_ui_text(pixels,pitch,50,140,"NO MESSAGES",1,0x0068FF68u);for(int i=0;i<count&&i<6;++i){char line[76];snprintf(line,sizeof(line),"%.12s: %.48s",rows[i].name,rows[i].text);menu_ui_text(pixels,pitch,38,120+i*40,line,1,0x0068FF68u);}menu_ui_text(pixels,pitch,34,397,lobby_default?"A MESSAGE  Y WORLD/LOBBY  X REFRESH  B BACK":"A MESSAGE  X REFRESH  B BACK",1,0x00C8D4DCu);}menu_present();uint16_t p=menu_pressed();if(p&ZMENU_B)break;if((p&(1u<<1))){refresh=true;}if(lobby_default&&(p&ZMENU_Y)){lobby=!lobby;refresh=true;}if(p&ZMENU_A){char text[49]={0};if(r499_keyboard(lobby?"LOBBY CHAT":"WORLD CHAT","MESSAGE",text,sizeof(text),48,false)){r499_chat_send(q,lobby,text);refresh=true;menu_latch_current();}}menu_yield();}closesocket(q);menu_latch_current();
}

static void public_try_commit_direct(void){
  if(!s_public_direct && s_public_local_direct_ok && s_public_peer_direct_ok){
    s_public_direct=true;
    Xbox_Log("ZNET R49 PUBLIC DIRECT bidirectional confirmed route=%s\n",s_public_lan_candidate?"LAN":"WAN");
    public_diag_flush();
  }
}

static void public_note_direct_rx(void){
  s_public_last_direct_rx=GetTickCount();
  if(s_public_direct_valid<3)++s_public_direct_valid;
  if(!s_public_local_direct_ok && s_public_direct_valid>=3){
    s_public_local_direct_ok=true;
    Xbox_Log("ZNET R49 PUBLIC direct inbound validated - awaiting peer confirm\n");
    public_diag_flush();
  }
  public_try_commit_direct();
}

static bool public_handle_nat(const char* b,const sockaddr_in& from){
  if(!s_public_mode||!b)return false;
  /* R48: "ZNAT1|INTRO|" is exactly 12 bytes.  R47.8 compared 13,
   * which included the literal NUL and rejected every valid INTRO whose
   * next byte was the first cookie character. */
  if(!strncmp(b,"ZNAT1|INTRO|",12)&&public_from_server(from)){
    char cookie[20]={0},ip[32]={0},role[8]={0},lanip[32]={0};unsigned port=0,lanport=0;
    int fields=sscanf(b,"ZNAT1|INTRO|%19[^|]|%31[^|]|%u|%7[^|]|LAN|%31[^|]|%u",cookie,ip,&port,role,lanip,&lanport);
    if(fields<4)fields=sscanf(b,"ZNAT1|INTRO|%19[^|]|%31[^|]|%u|%7s",cookie,ip,&port,role);
    if(fields>=4&&port>0&&port<65536){
      const char* use_ip=ip;unsigned use_port=port;bool lan=false;
      if(fields==6&&lanport>0&&lanport<65536&&inet_addr(lanip)!=INADDR_NONE){use_ip=lanip;use_port=lanport;lan=true;}
      sockaddr_in a;memset(&a,0,sizeof(a));a.sin_family=AF_INET;a.sin_port=htons((u_short)use_port);a.sin_addr.s_addr=inet_addr(use_ip);
      if(a.sin_addr.s_addr!=INADDR_NONE){bool changed=!s_public_have_intro||!peer_equal(a,s_public_peer);s_public_peer=a;s_public_have_intro=true;s_public_lan_candidate=lan;strncpy(s_public_peer_cookie,cookie,sizeof(s_public_peer_cookie)-1);s_public_peer_cookie[sizeof(s_public_peer_cookie)-1]=0;
        if(changed){s_public_direct=false;s_public_last_direct_rx=0;s_public_direct_valid=0;s_public_local_direct_ok=false;s_public_peer_direct_ok=false;Xbox_Log("ZNET R49 PUBLIC NAT intro peer=%s:%u role=%s route=%s\n",use_ip,use_port,role,lan?"LAN-CANDIDATE":"WAN-CANDIDATE");public_diag_flush();}}
    }return true;
  }
  if(!strncmp(b,"ZNAT1|SELF|",11)&&public_from_server(from)){char cookie[20]={0},role[8]={0};if(sscanf(b,"ZNAT1|SELF|%19[^|]|%7s",cookie,role)>=1){strncpy(s_public_cookie,cookie,sizeof(s_public_cookie)-1);s_public_cookie[sizeof(s_public_cookie)-1]=0;s_public_have_self=true;}return true;}
  if(!strcmp(b,"ZNAT1|RESET")&&public_from_server(from)){s_public_have_self=false;s_public_have_intro=false;s_public_direct=false;s_public_last_direct_rx=0;s_public_direct_valid=0;s_public_local_direct_ok=false;s_public_peer_direct_ok=false;Xbox_Log("ZNET R49 PUBLIC server route reset\n");public_diag_flush();return true;}
  if(!strncmp(b,"ZNAT1|PUNCH|",12)&&s_public_have_intro&&peer_equal(from,s_public_peer)){char cookie[20]={0};if(sscanf(b,"ZNAT1|PUNCH|%19s",cookie)==1&&(!s_public_peer_cookie[0]||!strcmp(cookie,s_public_peer_cookie)))public_note_direct_rx();return true;}
  if(!strncmp(b,"ZNAT1|DIRECTOK|",15)&&s_public_have_intro&&peer_equal(from,s_public_peer)){char cookie[20]={0};if(sscanf(b,"ZNAT1|DIRECTOK|%19s",cookie)==1&&(!s_public_peer_cookie[0]||!strcmp(cookie,s_public_peer_cookie))){public_note_direct_rx();if(!s_public_peer_direct_ok){s_public_peer_direct_ok=true;Xbox_Log("ZNET R49 PUBLIC peer direct confirm received\n");public_diag_flush();}public_try_commit_direct();}return true;}
  return false;
}

static int public_recv_ascii(char* out,int cap){
  if(!out||cap<2)return -1;
  for(int tries=0;tries<8;++tries){uint8_t raw[512];sockaddr_in from;int flen=sizeof(from);int n=recvfrom(s_sock,(char*)raw,sizeof(raw)-1,0,(sockaddr*)&from,&flen);
    if(n==SOCKET_ERROR){int e=WSAGetLastError();return e==WSAEWOULDBLOCK?0:-1;}raw[n]=0;const char* b=(const char*)raw;if(public_handle_nat(b,from))continue;
    if(public_from_server(from)&&!strncmp(b,"ZDIR1|",6)){int c=n<cap-1?n:cap-1;memcpy(out,b,c);out[c]=0;return 1;}}
  return 0;
}

static void public_tick(void){
  if(!s_public_mode||s_sock==INVALID_SOCKET)return;DWORD now=GetTickCount();
  if(s_public_direct&&s_public_last_direct_rx&&now-s_public_last_direct_rx>1800U){s_public_direct=false;s_public_last_direct_rx=0;s_public_direct_valid=0;s_public_local_direct_ok=false;s_public_peer_direct_ok=false;Xbox_Log("ZNET R49 PUBLIC DIRECT stale - relay resumed\n");public_diag_flush();}
  if(s_public_have_self&&(!s_public_last_keep||now-s_public_last_keep>=700U)){char m[80];snprintf(m,sizeof(m)-1,"ZNAT1|KEEP|%s",s_public_cookie);m[sizeof(m)-1]=0;public_send_text(m);s_public_last_keep=now;}
  if(s_public_host&&s_public_have_self&&s_public_room[0]&&s_public_token[0]&&(!s_public_last_heartbeat||now-s_public_last_heartbeat>=2500U)){char m[128];snprintf(m,sizeof(m)-1,"ZDIR1|HEARTBEAT|%s|%s|%s",s_public_room,s_public_token,s_public_cookie);m[sizeof(m)-1]=0;public_send_text(m);s_public_last_heartbeat=now;}
  if(s_public_have_intro&&s_public_have_self&&(!s_public_last_punch||now-s_public_last_punch>=120U)){char m[80];snprintf(m,sizeof(m)-1,"ZNAT1|PUNCH|%s",s_public_cookie);m[sizeof(m)-1]=0;sendto(s_sock,m,(int)strlen(m),0,(const sockaddr*)&s_public_peer,sizeof(s_public_peer));if(s_public_local_direct_ok){snprintf(m,sizeof(m)-1,"ZNAT1|DIRECTOK|%s",s_public_cookie);m[sizeof(m)-1]=0;sendto(s_sock,m,(int)strlen(m),0,(const sockaddr*)&s_public_peer,sizeof(s_public_peer));}s_public_last_punch=now;}
}

static bool public_relay_packet(const uint8_t* packet,int n){
  if(!s_public_mode||!s_public_have_self||!packet||n<=0||n>ZNP_MAX_PACKET)return false;uint8_t env[32+ZNP_MAX_PACKET];memcpy(env,"ZRLY1|",6);memcpy(env+6,s_public_cookie,16);env[22]='|';memcpy(env+23,packet,n);sockaddr_in d=public_directory_addr();int total=23+n;int r=sendto(s_sock,(const char*)env,total,0,(const sockaddr*)&d,sizeof(d));if(r==total){if(s_public_relay_tx==0){Xbox_Log("ZNET R49 PUBLIC relay TX active\n");public_diag_flush();}++s_public_relay_tx;return true;}return false;
}

static bool public_register_host(void){
  Xbox_Log("ZNET R49 PUBLIC register begin server=%s:%d build=%08lX\n",ZDIR_IP,ZDIR_PORT,(unsigned long)ZNP_BUILD);public_diag_flush();
  char msg[128],resp[256];DWORD begin=GetTickCount(),last=0;snprintf(msg,sizeof(msg)-1,"ZDIR1|REGISTER|%08lX|%08lX%08lX|%u|%s|%u|%s",(unsigned long)ZNP_BUILD,(unsigned long)(s_rom_hash>>32),(unsigned long)s_rom_hash,(unsigned)s_ruleset,s_local_ip,s_public_local_port,r499_profile_token());msg[sizeof(msg)-1]=0;
  while(GetTickCount()-begin<5000U){DWORD now=GetTickCount();if(!last||now-last>=300U){public_send_text(msg);last=now;}int rr=public_recv_ascii(resp,sizeof(resp));if(rr<0)return false;if(rr>0){char rid[12]={0},token[24]={0},cookie[20]={0};if(sscanf(resp,"ZDIR1|REGOK|%11[^|]|%23[^|]|%19s",rid,token,cookie)==3){strncpy(s_public_room,rid,sizeof(s_public_room)-1);strncpy(s_public_token,token,sizeof(s_public_token)-1);strncpy(s_public_cookie,cookie,sizeof(s_public_cookie)-1);s_public_have_self=true;s_public_host=true;Xbox_Log("ZNET R49 PUBLIC room registered id=%s server=%s:%d\n",s_public_room,ZDIR_IP,ZDIR_PORT);public_diag_flush();return true;}if(strstr(resp,"ZDIR1|ERR|")){Xbox_Log("ZNET R49 PUBLIC register error %s\n",resp);public_diag_flush();return false;}}
    menu_frame("HOST PUBLIC ROOM","REGISTERING ROOM","PUBLIC DIRECTORY",ZDIR_IP,"AUTO NAT + RELAY","B CANCEL");if(menu_pressed()&ZMENU_B)return false;menu_yield();}return false;
}

static int public_fetch_rooms(PublicRoomEntry rooms[ZDIR_MAX_ROOMS]){
  R499BlockingAudioMute audio_mute;
  Xbox_Log("ZNET R49 PUBLIC LIST request build=%08lX\n",(unsigned long)ZNP_BUILD);public_diag_flush();
  char query[128],resp[256];snprintf(query,sizeof(query)-1,"ZDIR1|LIST|%08lX|%08lX%08lX",(unsigned long)ZNP_BUILD,(unsigned long)(s_rom_hash>>32),(unsigned long)s_rom_hash);query[sizeof(query)-1]=0;DWORD begin=GetTickCount(),last=0;int count=0;bool begun=false;
  while(GetTickCount()-begin<1800U){DWORD now=GetTickCount();if(!last||now-last>=400U){public_send_text(query);last=now;}int rr=public_recv_ascii(resp,sizeof(resp));if(rr<0)break;if(rr>0){if(!strncmp(resp,"ZDIR1|LISTBEGIN|",16)){count=0;begun=true;continue;}if(!strcmp(resp,"ZDIR1|LISTEND")){Xbox_Log("ZNET R49 PUBLIC LIST result rooms=%d\n",count);public_diag_flush();return count;}if(begun&&!strncmp(resp,"ZDIR1|ROOM|",11)&&count<ZDIR_MAX_ROOMS){PublicRoomEntry r;memset(&r,0,sizeof(r));if(sscanf(resp,"ZDIR1|ROOM|%11[^|]|%31[^|]|%u|%u|%u",r.id,r.name,&r.players,&r.max_players,&r.ruleset)>=4){r.ruleset&=3u;rooms[count++]=r;}}}Sleep(5);}return count;
}

static bool public_join_room_id(const char* rid,sockaddr_in* target){
  Xbox_Log("ZNET R49 PUBLIC JOIN begin room=%s\n",rid?rid:"?");public_diag_flush();
  if(!rid||!target)return false;char msg[160],resp[256];DWORD begin=GetTickCount(),last=0;bool joined=false;snprintf(msg,sizeof(msg)-1,"ZDIR1|JOIN|%s|%08lX|%08lX%08lX|%s|%u|%s",rid,(unsigned long)ZNP_BUILD,(unsigned long)(s_rom_hash>>32),(unsigned long)s_rom_hash,s_local_ip,s_public_local_port,r499_profile_token());msg[sizeof(msg)-1]=0;
  while(GetTickCount()-begin<7000U){DWORD now=GetTickCount();if(!joined&&(!last||now-last>=300U)){public_send_text(msg);last=now;}int rr=public_recv_ascii(resp,sizeof(resp));if(rr<0)return false;if(rr>0){char rrid[12]={0},cookie[20]={0};if(sscanf(resp,"ZDIR1|JOINOK|%11[^|]|%19s",rrid,cookie)==2){strncpy(s_public_room,rrid,sizeof(s_public_room)-1);strncpy(s_public_cookie,cookie,sizeof(s_public_cookie)-1);s_public_have_self=true;joined=true;}else if(strstr(resp,"ZDIR1|ERR|")){menu_frame("PUBLIC ROOMS","ROOM JOIN FAILED",resp+10,"REFRESH ROOM LIST","","B BACK");Sleep(900);return false;}}public_tick();if(joined&&s_public_have_intro){*target=s_public_peer;Xbox_Log("ZNET R49 PUBLIC joined room=%s\n",s_public_room);public_diag_flush();return true;}menu_frame("JOIN PUBLIC ROOM","JOINING ROOM",rid,joined?"FINDING PEER":"CONTACTING SERVER","AUTO NAT + RELAY","B CANCEL");if(menu_pressed()&ZMENU_B)return false;menu_yield();}return false;
}

static bool public_browse_rooms(sockaddr_in* target){
  int selected=0;menu_latch_current();
  Xbox_Log("ZNET R49 PUBLIC room browser opened\n");public_diag_flush();for(;;){PublicRoomEntry rooms[ZDIR_MAX_ROOMS];memset(rooms,0,sizeof(rooms));int count=public_fetch_rooms(rooms);if(count<=0){for(;;){menu_frame("PUBLIC ROOMS","NO OPEN ROOMS FOUND","","","","A REFRESH   B BACK");uint16_t p=menu_pressed();if(p&ZMENU_B)return false;if(p&ZMENU_A)break;menu_yield();}continue;}if(selected>=count)selected=count-1;
    for(;;){char a[64]="",b[64]="",c[64]="",d[64]="";int first=selected-1;if(first<0)first=0;if(first>count-3)first=count-3;if(first<0)first=0;char* line[3]={a,b,c};for(int j=0;j<3;++j){int i=first+j;if(i>=count)continue;snprintf(line[j],63,"%c %s %u/%u  %.12s",i==selected?'>':' ',rooms[i].name,rooms[i].players,rooms[i].max_players,r498_ruleset_name((uint8_t)rooms[i].ruleset));line[j][63]=0;}snprintf(d,sizeof(d)-1,"ROOM ID %s",rooms[selected].id);d[sizeof(d)-1]=0;menu_frame("PUBLIC ROOMS",a,b,c,d,"A JOIN   START REFRESH   B BACK");uint16_t p=menu_pressed();if(p&ZMENU_B)return false;if(p&ZMENU_START)break;if(p&ZMENU_UP)selected=(selected+count-1)%count;if(p&ZMENU_DOWN)selected=(selected+1)%count;if(p&ZMENU_A){s_ruleset=(uint8_t)(rooms[selected].ruleset&3u);if(public_join_room_id(rooms[selected].id,target))return true;break;}menu_yield();}}
}

static void public_leave(void){if(s_public_mode&&s_public_have_self&&s_public_cookie[0]){Xbox_Log("ZNET R49 PUBLIC leave room=%s role=%s\n",s_public_room,s_public_host?"HOST":"JOIN");public_diag_flush();char m[80];snprintf(m,sizeof(m)-1,"ZDIR1|LEAVE|%s",s_public_cookie);m[sizeof(m)-1]=0;for(int i=0;i<2;++i)public_send_text(m);}}

static int make_packet(uint8_t* out, int cap, uint8_t type, uint32_t session,
                       const uint8_t* payload, int payload_len) {
  int n = ZNP_HEADER + payload_len;
  if (!out || n > cap || payload_len < 0) return 0;
  out[0] = ZNP_MAGIC0; out[1] = ZNP_MAGIC1; out[2] = ZNP_MAGIC2; out[3] = ZNP_MAGIC3;
  out[4] = ZNP_PROTOCOL; out[5] = type; put16(out + 6, (uint16_t)n);
  put32(out + 8, ZNP_BUILD); put32(out + 12, session);
  put32(out + 16, s_seq++); put32(out + 20, s_core.frame);
  if (payload_len) memcpy(out + ZNP_HEADER, payload, (size_t)payload_len);
  return n;
}

static bool send_raw_to(uint8_t type, uint32_t session, const uint8_t* payload,
                        int payload_len, const sockaddr_in& to) {
  uint8_t packet[ZNP_MAX_PACKET];int n=make_packet(packet,sizeof(packet),type,session,payload,payload_len);if(!n)return false;
  int r=sendto(s_sock,(const char*)packet,n,0,(const sockaddr*)&to,sizeof(to));bool direct_ok=(r==n),relay_ok=false;if(s_public_mode&&!s_public_direct)relay_ok=public_relay_packet(packet,n);if(!direct_ok&&!relay_ok)return false;++s_stats.tx_packets;s_last_tx=GetTickCount();return true;
}

static bool send_peer(uint8_t type, const uint8_t* payload, int payload_len) {
  return s_peer_known && send_raw_to(type, s_session, payload, payload_len, s_peer);
}

struct ParsedPacket {
  uint8_t type;
  uint32_t session;
  uint32_t seq;
  uint32_t ack;
  const uint8_t* payload;
  int payload_len;
};

static bool parse_packet(const uint8_t* p, int n, ParsedPacket* out) {
  if (!p || n < ZNP_HEADER || !out) return false;
  if (p[0] != ZNP_MAGIC0 || p[1] != ZNP_MAGIC1 || p[2] != ZNP_MAGIC2 || p[3] != ZNP_MAGIC3) return false;
  if (p[4] != ZNP_PROTOCOL || get16(p + 6) != n || get32(p + 8) != ZNP_BUILD) return false;
  out->type = p[5]; out->session = get32(p + 12); out->seq = get32(p + 16);
  out->ack = get32(p + 20); out->payload = p + ZNP_HEADER;
  out->payload_len = n - ZNP_HEADER;
  return true;
}

static int recv_one(ParsedPacket* pp, sockaddr_in* from, uint8_t* storage) {
  for(int tries=0;tries<12;++tries){bool via_relay=false;int flen=sizeof(*from);int n=recvfrom(s_sock,(char*)storage,ZNP_MAX_PACKET,0,(sockaddr*)from,&flen);if(n==SOCKET_ERROR){int e=WSAGetLastError();if(e==WSAEWOULDBLOCK)return 0;return -1;}
    if(s_public_mode){
      if(n>=23&&public_from_server(*from)&&!memcmp(storage,"ZRLY1|",6)&&storage[22]=='|'){
        if(s_public_peer_cookie[0]&&memcmp(storage+6,s_public_peer_cookie,16)!=0)continue;int payload_n=n-23;if(payload_n<=0||payload_n>ZNP_MAX_PACKET)continue;memmove(storage,storage+23,(size_t)payload_n);n=payload_n;if(!s_public_have_intro)continue;*from=s_public_peer;via_relay=true;if(s_public_relay_rx==0){Xbox_Log("ZNET R49 PUBLIC relay RX active\n");public_diag_flush();}++s_public_relay_rx;
      }else if(n>0&&storage[0]=='Z'){
        char b[300];int c=n<(int)sizeof(b)-1?n:(int)sizeof(b)-1;memcpy(b,storage,c);b[c]=0;if(public_handle_nat(b,*from))continue;if(public_from_server(*from)&&!strncmp(b,"ZDIR1|",6))continue;
      }
    }
    if(!parse_packet(storage,n,pp))continue;if(s_public_mode&&!via_relay&&s_public_have_intro&&peer_equal(*from,s_public_peer))public_note_direct_rx();++s_stats.rx_packets;s_last_rx=GetTickCount();return 1;
  }return 0;
}

/* R48 AUTO delay smoothing ------------------------------------------------
 * LAN DIRECT keeps the proven R47.14/R47.15 tiers and hysteresis.
 * WAN/relay now has a longer calibration settle period so NAT/handshake RTT
 * spikes cannot push AUTO to 12F before the actual route stabilizes. After the
 * settle window, high-latency WAN uses the sustained rolling median with an
 * empirical one-way-frame estimate validated by the long R47.15 hardware run:
 *   - LAN DIRECT: <=35 / <=60 / <=90 / >90 ms => 1 / 2 / 3 / 4 frames
 *   - WAN/relay <=85 ms: 1 / 2 / 3 frames (+ sustained jitter safety)
 *   - WAN/relay >85 ms: round((RTT/2) / 16.67 ms), floor 4F, cap 12F
 *   - stable jitter >=30 ms adds 1F; >=60 ms adds a second safety frame
 *   - WAN/relay route settle: 2500 ms; LAN route settle remains 750 ms
 *   - after initial WAN settle, the first 9-sample median commits immediately
 *   - later increases need 8 matching candidates; decreases need 24
 *
 * R49 ROLLBACK AUTO only: 180..210ms targets 5F coverage split into 4F+1RB;
 * jitter >=30/60ms adds one/two base frames. Other WAN tiers retain their
 * calibrated coverage with one rollback frame. DELAY/manual retain old policy.
 * Hardware evidence for the original DELAY calibration:
 *   ~104 ms / 5 ms  -> 4F
 *   ~201 ms / 18 ms -> 6F AUTO target (5F manual ran >82k frames, fault=0)
 *   ~228 ms / 18 ms -> 7F AUTO target
 * Manual 0..12F remains available for explicit testing/override.
 */
#define ZNP_AUTO_WINDOW 9
struct AutoDelayFilter {
  uint16_t rtt[ZNP_AUTO_WINDOW];
  uint8_t count;
  uint8_t pos;
  uint8_t stable;
  uint8_t pending;
  uint8_t pending_count;
  uint8_t calibrated;
};

static void auto_delay_reset(AutoDelayFilter* f) {
  if (!f) return;
  memset(f, 0, sizeof(*f));
  f->stable = 1;
  f->pending = 1;
  f->calibrated = 0;
}

static uint32_t auto_delay_median(const AutoDelayFilter* f) {
  if (!f || !f->count) return 0;
  uint16_t tmp[ZNP_AUTO_WINDOW];
  int n = (int)f->count;
  for (int i = 0; i < n; ++i) tmp[i] = f->rtt[i];
  for (int i = 1; i < n; ++i) {
    uint16_t v = tmp[i]; int j = i - 1;
    while (j >= 0 && tmp[j] > v) { tmp[j + 1] = tmp[j]; --j; }
    tmp[j + 1] = v;
  }
  return tmp[n / 2];
}

static uint8_t auto_delay_candidate(uint32_t median_rtt_ms, uint32_t jitter_ms,
                                    bool lan_direct, bool shallow_rollback) {
  uint32_t frames;
  if (lan_direct) {
    // Preserve the proven R47.14 Xbox<->Xemu LAN behavior exactly.
    frames = median_rtt_ms <= 35u ? 1u : median_rtt_ms <= 60u ? 2u : median_rtt_ms <= 90u ? 3u : 4u;
    if (jitter_ms >= 20u && frames < 4u) ++frames;
  } else if (median_rtt_ms <= 85u) {
    // Preserve the existing low/medium-latency WAN tiers.
    frames = median_rtt_ms <= 25u ? 1u : median_rtt_ms <= 55u ? 2u : 3u;
    if (jitter_ms >= 20u && frames < 4u) ++frames;
  } else {
    /* R47.15 proved that the conservative RTT/2+jitter deadline formula was
     * over-buffering real hardware.  A 201 ms / 18 ms WAN DIRECT session ran
     * for >82k synchronized frames at manual 5F with fault=0 and was mostly
     * 60 FPS.  AUTO therefore targets the nearest one-way frame count instead
     * of adding the full jitter term to every frame of lead.  A small safety
     * bump is reserved for sustained, actually-bad jitter.
     *
     * round((RTT/2) / 16.6667ms) == round(RTT * 3 / 100).
     */
    // R49 hardware-test target: 4F real delay + 1 predicted frame.
    // Leave pure DELAY calibration unchanged; sustained jitter adds base delay.
    frames = shallow_rollback && median_rtt_ms >= 180u && median_rtt_ms <= 210u
           ? 5u : (median_rtt_ms * 3u + 50u) / 100u;
    if (frames < 4u) frames = 4u;
    if (jitter_ms >= 30u && frames < 12u) ++frames;
    if (jitter_ms >= 60u && frames < 12u) ++frames;
    if (frames > 12u) frames = 12u;
  }
  return (uint8_t)frames;
}

static uint8_t auto_delay_update(AutoDelayFilter* f, uint32_t sample_ms,
                                 uint32_t jitter_ms, bool lan_direct,
                                 bool allow_transition, bool fast_initial_wan, bool shallow_rollback,
                                 uint32_t* median_out, bool* changed_out) {
  if (changed_out) *changed_out = false;
  if (!f) return 1;
  if (sample_ms > 65535u) sample_ms = 65535u;
  f->rtt[f->pos] = (uint16_t)sample_ms;
  f->pos = (uint8_t)((f->pos + 1u) % ZNP_AUTO_WINDOW);
  if (f->count < ZNP_AUTO_WINDOW) ++f->count;

  uint32_t median = auto_delay_median(f);
  if (median_out) *median_out = median;
  uint8_t candidate = auto_delay_candidate(median, jitter_ms, lan_direct, shallow_rollback);
  if (!allow_transition) {
    // Keep filling the rolling window, but do not let relay/NAT handshake
    // samples select a gameplay delay before the route has settled.
    f->stable = 1;
    f->pending = 1;
    f->pending_count = 0;
    f->calibrated = 0;
    return f->stable;
  }

  // WAN/relay has already spent 2.5s settling.  Its final 9 samples are much
  // more representative than the old startup EWMA, so commit that first
  // sustained candidate immediately instead of waiting another 400ms.
  if (!f->calibrated && fast_initial_wan && f->count >= ZNP_AUTO_WINDOW) {
    uint8_t old = f->stable;
    f->stable = candidate;
    f->pending = candidate;
    f->pending_count = 0;
    f->calibrated = 1;
    if (changed_out && old != candidate) *changed_out = true;
    return f->stable;
  }
  if (!f->calibrated) f->calibrated = 1;

  if (candidate == f->stable) {
    f->pending = candidate;
    f->pending_count = 0;
    return f->stable;
  }

  if (candidate != f->pending) {
    f->pending = candidate;
    f->pending_count = 1;
  } else if (f->pending_count < 255u) {
    ++f->pending_count;
  }

  const uint8_t need = candidate > f->stable ? 8u : 24u;
  if (f->pending_count >= need) {
    f->stable = candidate;
    f->pending_count = 0;
    if (changed_out) *changed_out = true;
  }
  return f->stable;
}

static int step_delay_option(int option, int direction) {
  // option -1=AUTO, otherwise an exact manual delay 0..12.
  if (direction > 0) {
    if (option < 0) return 0;
    if (option < 12) return option + 1;
    return -1;
  }
  if (option < 0) return 12;
  if (option > 0) return option - 1;
  return -1;
}

static bool ipv4_is_private_lan(uint32_t network_order_addr) {
  const uint32_t h = ntohl(network_order_addr);
  if ((h & 0xff000000u) == 0x0a000000u) return true;   // 10.0.0.0/8
  if ((h & 0xfff00000u) == 0xac100000u) return true;   // 172.16.0.0/12
  if ((h & 0xffff0000u) == 0xc0a80000u) return true;   // 192.168.0.0/16
  if ((h & 0xffff0000u) == 0xa9fe0000u) return true;   // 169.254.0.0/16
  return false;
}

static bool host_route_is_confirmed_lan(bool public_host) {
  if (public_host)
    return s_public_direct && s_public_lan_candidate;
  return s_peer_known && ipv4_is_private_lan(s_peer.sin_addr.s_addr);
}

// R49.3: MANUAL sends exactly the selected delay and independent RB bit.
static uint8_t clamp_delay(int value) { return (uint8_t)(value < 0 ? 0 : value > 12 ? 12 : value); }
static uint8_t clamp_rollback(int value) { return (uint8_t)(value <= 0 ? 0 : 1); }
static void negotiated_pair(int option, int automatic, bool rb, int* delay, uint8_t* window) {
  *window = rb ? 1 : 0;
  // Preserve the calibrated R49 WAN coverage policy only in AUTO.
  *delay = clamp_delay(option < 0 ? automatic - (rb && automatic > 1 ? 1 : 0) : option);
}
void Xbox_Netplay_SetFrontend(XboxNetplayBackground background, XboxNetplayPrepare prepare) {
  s_background = background; s_prepare = prepare;
}

static bool wait_host_handshake(bool public_host) {
  uint8_t buf[ZNP_MAX_PACKET], payload[64];
  DWORD announce = 0, last_offer = 0, last_ping = 0;
  bool got_ready = false, got_ack = false, got_boot = false, start_requested = false;
  bool start_queued=false;int prepare_progress=0;DWORD start_tick=0;
  int delay_option = -1;  // AUTO by default.
  int delay = 1;             // total coverage target chosen by AUTO/manual
  int game_delay = 1;        // actual local input delay (rollback subtracts prediction window)
  uint8_t rollback_window = 0;
  bool rollback_mode = false;
  bool effective_rollback_mode = false;
  bool lan_fast_bypass = false;
  uint32_t rtt_avg = 0, rtt_median = 0, jitter = 0, samples = 0, ping_id = 1;
  uint8_t auto_delay = 1;
  bool auto_ready = false;
  AutoDelayFilter auto_filter; auto_delay_reset(&auto_filter);
  bool calibrated_direct = public_host ? s_public_direct : false;
  DWORD auto_route_warmup_until = 0;
  char local[32]; local_ip_text(local,sizeof(local));
  if(!public_host){
    menu_frame("HOST DIRECT","LOADING...","CHECKING NETWORK","PUBLIC IP DISCOVERY","","PLEASE WAIT");
    { R499BlockingAudioMute audio_mute; discover_public_ip(); }
  }
  if(public_host)Xbox_Log("ZNET R49 PUBLIC HOST room=%s delay=AUTO/%d rom=%08lX%08lX\n",s_public_room,delay,(unsigned long)(s_rom_hash>>32),(unsigned long)s_rom_hash);
  else Xbox_Log("ZNET R49 HOST DIRECT lobby udp/%d delay=AUTO/%d local=%s rom=%08lX%08lX\n",ZNP_PORT,delay,local,(unsigned long)(s_rom_hash>>32),(unsigned long)s_rom_hash);
  menu_latch_current();
  for (;;) {
    if(public_host){
      public_tick();
      if(s_public_direct!=calibrated_direct){
        calibrated_direct=s_public_direct;rtt_avg=0;rtt_median=0;jitter=0;samples=0;auto_delay=1;auto_ready=false;auto_delay_reset(&auto_filter);
        auto_route_warmup_until=GetTickCount() +
          (s_public_direct && s_public_lan_candidate ? ZNP_AUTO_ROUTE_WARMUP_LAN_MS
                                                     : ZNP_AUTO_ROUTE_WARMUP_WAN_MS);
        s_stats.rtt_ms=0;s_stats.jitter_ms=0;s_stats.calibration_samples=0;s_stats.auto_delay=1;
        Xbox_Log("ZNET R49 PUBLIC route changed to %s - RTT calibration reset\n",s_public_direct?(s_public_lan_candidate?"LAN DIRECT":"WAN DIRECT"):"SERVER RELAY");
        public_diag_flush();
      }
    }
    for(int packet=0;packet<32;++packet) {
    ParsedPacket pp; sockaddr_in from; memset(&from, 0, sizeof(from));
    int rr = recv_one(&pp, &from, buf);
    if (rr < 0) return false;
    if (rr == 0) break;
    if (rr > 0) {
      if (pp.type == ZNP_HELLO && pp.payload_len >= 8) {
        if(public_host&&(!s_public_have_intro||!peer_equal(from,s_public_peer)))continue;
        uint64_t rh = get64(pp.payload);
        if (rh != s_rom_hash) {
          uint8_t why = 1; send_raw_to(ZNP_REJECT, 0, &why, 1, from);
          continue;
        }
        if(pp.payload_len>=9){unsigned n=pp.payload[8];if(n>R498_PROFILE_NAME_MAX)n=R498_PROFILE_NAME_MAX;if((int)(9+n)<=pp.payload_len){memcpy(s_peer_profile,pp.payload+9,n);s_peer_profile[n]=0;}}
        if (!s_peer_known) {
          s_peer = from; s_peer_known = true;
          s_session = (uint32_t)GetTickCount() ^ (uint32_t)__builtin_readcyclecounter() ^ 0x47414D5Au;
          if (!s_session) s_session = 0x47400001u;
          char peerip[32]; ipv4_text(ntohl(s_peer.sin_addr.s_addr),peerip,sizeof(peerip));
          Xbox_Log("ZNET R49 HELLO accepted peer=%s session=%08lX\n",peerip,(unsigned long)s_session);
          if (!auto_route_warmup_until) {
            const bool lan_now = public_host && s_public_direct && s_public_lan_candidate;
            auto_route_warmup_until = GetTickCount() +
              (lan_now ? ZNP_AUTO_ROUTE_WARMUP_LAN_MS : ZNP_AUTO_ROUTE_WARMUP_WAN_MS);
          }
          last_offer = 0; last_ping = 0;
        }
      } else if (s_peer_known && peer_equal(from,s_peer) && pp.session==s_session) {
        if (pp.type==ZNP_READY) got_ready=true;
        else if (pp.type==ZNP_START_ACK) got_ack=true;
        else if (pp.type==ZNP_BOOT_READY) got_boot=true;
        else if (pp.type==ZNP_PONG && pp.payload_len>=8) {
          DWORD now_ping = GetTickCount();
          uint32_t sent = get32(pp.payload);
          uint32_t sample = (uint32_t)(now_ping - sent);
          if (sample <= 2000u) {
            if (samples == 0) {
              rtt_avg = sample;
              jitter = 0;
            } else {
              uint32_t diff = sample > rtt_avg ? sample - rtt_avg : rtt_avg - sample;
              rtt_avg = (rtt_avg * 7u + sample + 4u) / 8u;
              jitter = (jitter * 3u + diff + 2u) / 4u;
            }
            ++samples;
            bool auto_changed=false;
            uint8_t old_auto=auto_delay;
            const bool auto_transition_ok = !auto_route_warmup_until ||
              (int32_t)(now_ping - auto_route_warmup_until) >= 0;
            const bool auto_lan_direct = host_route_is_confirmed_lan(public_host);
            auto_delay = auto_delay_update(&auto_filter,sample,jitter,
                                           auto_lan_direct, auto_transition_ok,
                                           !auto_lan_direct, rollback_mode,
                                           &rtt_median,&auto_changed);
            auto_ready = auto_filter.calibrated != 0;
            s_stats.rtt_ms=rtt_avg; s_stats.jitter_ms=jitter;
            s_stats.calibration_samples=samples; s_stats.auto_delay=auto_delay;
            if(auto_changed){
              Xbox_Log("ZNET R49 AUTO delay %u->%u median=%lums ewma=%lums jitter=%lums route=%s settled=%u\n",
                       (unsigned)old_auto,(unsigned)auto_delay,(unsigned long)rtt_median,
                       (unsigned long)rtt_avg,(unsigned long)jitter,
                       public_host?(s_public_direct?(s_public_lan_candidate?"LAN DIRECT":"WAN DIRECT"):"SERVER RELAY"):"DIRECT",
                       (unsigned)(auto_transition_ok ? 1u : 0u));
              public_diag_flush();
            }
          }
        }
      }
    }

    } // drain lobby packets before simulation/render
    DWORD now=GetTickCount();
    if (!start_requested) {
      if (delay_option < 0) delay = (int)auto_delay;
      lan_fast_bypass = false;
      negotiated_pair(delay_option, auto_delay, rollback_mode, &game_delay, &rollback_window);
      effective_rollback_mode = rollback_mode && rollback_window != 0;
    }

    if (s_peer_known && !start_requested && now-last_offer>=100) {
      put64(payload,s_rom_hash);
      payload[8]=(uint8_t)game_delay;
      payload[9]=(uint8_t)(delay_option<0?1:0);
      payload[10]=(uint8_t)(effective_rollback_mode?1:0);
      payload[11]=rollback_window;
      put16(payload+12,(uint16_t)(rtt_avg>65535u?65535u:rtt_avg));
      put16(payload+14,(uint16_t)(jitter>65535u?65535u:jitter));
      payload[16]=s_ruleset;
      {const char* pn=Xbox_Netplay_ProfileName();unsigned n=(unsigned)strlen(pn);if(n>R498_PROFILE_NAME_MAX)n=R498_PROFILE_NAME_MAX;payload[17]=(uint8_t)n;memcpy(payload+18,pn,n);send_peer(ZNP_OFFER,payload,(int)(18+n));}
      last_offer=now;
    }
    if (s_peer_known && !start_requested && now-last_ping>=50) {
      put32(payload,(uint32_t)now); put32(payload+4,ping_id++);
      send_peer(ZNP_PING,payload,8);
      last_ping=now;
    }

    char publicline[64],localline[64],status[64],delayline[96];
    if(public_host){snprintf(publicline,sizeof(publicline),"PUBLIC ROOM %s",s_public_room);snprintf(localline,sizeof(localline),"ROUTE %s",s_public_direct?(s_public_lan_candidate?"LAN DIRECT":"WAN DIRECT"):(s_public_relay_rx?"SERVER RELAY":"AUTO NAT / RELAY"));}
    else {snprintf(publicline,sizeof(publicline),"PUBLIC IP %s",s_public_ip);snprintf(localline,sizeof(localline),"LOCAL IP %s",local);}
    snprintf(delayline,sizeof(delayline),"%s\nDELAY %s %dF\nROLLBACK: %s",r498_ruleset_name(s_ruleset),delay_option<0?"AUTO":"MANUAL",game_delay,effective_rollback_mode?"ENABLED":"DISABLED");
    if(!s_peer_known)strcpy(status,"PLAYER 2 WAITING");
    else if(!got_ready)strcpy(status,"PLAYER 2 CONNECTING");
    else if(!start_requested && delay_option<0 && !auto_ready)strcpy(status,"AUTO CALIBRATING - PLEASE WAIT");
    else if(!start_requested)snprintf(status,sizeof(status),"P2 %.12s READY - PRESS A",Xbox_Netplay_PeerProfileName());
    else if(prepare_progress<100)snprintf(status,sizeof(status),"PREPARING GAME %d%%",prepare_progress);
    else strcpy(status,"WAITING FOR PEER TO FINISH PREPARING");
    if(start_queued && !start_requested)strcpy(status,got_ready?"START QUEUED - AUTO CALIBRATING":"START QUEUED - WAITING FOR PLAYER 2");
    const char* footer=public_host?"LEFT/RIGHT DELAY  RS ROLLBACK\nY CHAT  A/START BEGIN  B BACK":"LEFT/RIGHT DELAY  RS ROLLBACK\nA/START BEGIN  B BACK";
    menu_frame(public_host?"HOST PUBLIC ROOM":"HOST DIRECT",publicline,localline,status,delayline,footer);
    uint16_t mp=menu_pressed();
    if(mp&ZMENU_B){Xbox_Log("ZNET R49 HOST cancelled\n");return false;}
    if(public_host && (mp&ZMENU_Y) && !start_requested){r499_chat_menu(true);menu_latch_current();continue;}
    if(!start_requested){
      if(mp&ZMENU_LEFT) delay_option=step_delay_option(delay_option,-1);
      if(mp&ZMENU_RIGHT) delay_option=step_delay_option(delay_option,1);
      if(mp&ZMENU_RS){rollback_mode=!rollback_mode;last_offer=0;}
      if(delay_option < 0) delay=(int)auto_delay; else delay=delay_option;
      lan_fast_bypass = false;
      negotiated_pair(delay_option, auto_delay, rollback_mode, &game_delay, &rollback_window);
      effective_rollback_mode = rollback_mode && rollback_window != 0;
      if(mp&(ZMENU_A|ZMENU_START)) {
        start_queued=true;
        Xbox_Log("ZNET R49.6 start queued ready=%u calibrated=%u\n",(unsigned)got_ready,(unsigned)auto_ready);
        public_diag_flush();
      }
      if(start_queued && got_ready && (delay_option>=0 || auto_ready)){
        start_requested=true;got_ack=false;got_boot=false;announce=0;start_tick=GetTickCount();
        prepare_progress=s_prepare?s_prepare(true):100;
        if(prepare_progress<0)return false;
        s_stats.delay_auto=(uint8_t)(delay_option<0?1:0);
        s_stats.auto_delay=auto_delay;s_stats.delay=(uint8_t)delay;
        s_stats.rtt_ms=rtt_avg;s_stats.jitter_ms=jitter;s_stats.calibration_samples=samples;
        Xbox_Log("ZNET R49 HOST start pressed coverage=%d base=%d rb=%u requested=%s netcode=%s mode=%s lan-bypass=%u rtt=%lums median=%lums jitter=%lums samples=%lu\n",
                 delay,game_delay,(unsigned)rollback_window,rollback_mode?"ROLLBACK":"DELAY",
                 effective_rollback_mode?"ROLLBACK":(lan_fast_bypass?"LAN-FAST":"DELAY"),
                 delay_option<0?"AUTO":"MANUAL",(unsigned)(lan_fast_bypass?1u:0u),(unsigned long)rtt_avg,
                 (unsigned long)rtt_median,(unsigned long)jitter,(unsigned long)samples);
      }
    }

    if(start_requested && (announce==0 || now-announce>=100)){
      payload[0]=(uint8_t)game_delay;payload[1]=2;payload[2]=0;payload[3]=1;
      payload[4]=(uint8_t)(effective_rollback_mode?1:0);payload[5]=rollback_window;payload[6]=s_ruleset;
      send_peer(ZNP_START,payload,7);announce=now;
    }
    if(start_requested && prepare_progress<100) {
      prepare_progress=s_prepare?s_prepare(false):100;
      if(prepare_progress<0)return false;
    }
    if(start_requested && GetTickCount()-start_tick>120000) {
      Xbox_Log("ZNET R49.6 start timeout local=%d peer-ready=%u\n",prepare_progress,(unsigned)got_boot);
      public_diag_flush();return false;
    }
    if(start_requested&&prepare_progress==100&&got_ack&&got_boot){
      payload[0]=(uint8_t)game_delay;payload[1]=(uint8_t)(effective_rollback_mode?1:0);payload[2]=rollback_window;payload[3]=s_ruleset;for(int i=0;i<4;++i)send_peer(ZNP_BOOT_GO,payload,4);
      znp_core_init(&s_core,0,(uint8_t)game_delay);s_stats.delay=(uint8_t)game_delay;s_stats.local_slot=0;s_stats.active=1;
      s_rollback_enabled=effective_rollback_mode;s_rollback_window=rollback_window;rollback_reset((uint8_t)game_delay,rollback_window);
      Xbox_Log("ZNET R49 START host slot=P1 peer=P2 coverage=%d base=%d rb=%u requested=%s netcode=%s mode=%s auto=%u lan-bypass=%u rtt=%lums median=%lums jitter=%lums\n",
               delay,game_delay,(unsigned)rollback_window,rollback_mode?"ROLLBACK":"DELAY",
               effective_rollback_mode?"ROLLBACK":(lan_fast_bypass?"LAN-FAST":"DELAY"),
               s_stats.delay_auto?"AUTO":"MANUAL",(unsigned)auto_delay,(unsigned)(lan_fast_bypass?1u:0u),
               (unsigned long)rtt_avg,(unsigned long)rtt_median,(unsigned long)jitter);return true;
    }
    menu_yield();
  }
}

static bool wait_join_handshake(const sockaddr_in& target, bool public_join) {
  uint8_t buf[ZNP_MAX_PACKET], payload[64];
  DWORD last_hello=0,last_ready=0,start_tick=GetTickCount();
  int delay=1;bool offered=false,started=false,delay_auto=true;int prepare_progress=0;
  bool rollback_mode=false;uint8_t rollback_window=0;
  uint32_t rtt_ms=0,jitter_ms=0,samples=0;
  char hostip[32];ipv4_text(ntohl(target.sin_addr.s_addr),hostip,sizeof(hostip));
  Xbox_Log("ZNET R49 JOIN target=%s:%d rom=%08lX%08lX\n",hostip,ZNP_PORT,(unsigned long)(s_rom_hash>>32),(unsigned long)s_rom_hash);
  menu_latch_current();
  for(;;){
    if(public_join)public_tick();
    DWORD now=GetTickCount();
    if(!offered&&now-last_hello>=250){put64(payload,s_rom_hash);const char* pn=Xbox_Netplay_ProfileName();unsigned n=(unsigned)strlen(pn);if(n>R498_PROFILE_NAME_MAX)n=R498_PROFILE_NAME_MAX;payload[8]=(uint8_t)n;memcpy(payload+9,pn,n);send_raw_to(ZNP_HELLO,0,payload,(int)(9+n),target);last_hello=now;}
    if(offered&&!started&&now-last_ready>=100){send_peer(ZNP_READY,0,0);last_ready=now;}
    for(int packet=0;packet<32;++packet) {
    ParsedPacket pp;sockaddr_in from;memset(&from,0,sizeof(from));int rr=recv_one(&pp,&from,buf);if(rr<0)return false;if(rr==0)break;
    if(rr>0&&peer_equal(from,target)){
      if(pp.type==ZNP_REJECT){menu_frame("JOIN DIRECT","HOST REJECTED CONNECTION","ROM OR BUILD MISMATCH","","","B BACK");Sleep(900);return false;}
      if(!started&&pp.type==ZNP_OFFER&&pp.payload_len>=18&&get64(pp.payload)==s_rom_hash){
        s_peer=from;s_peer_known=true;s_session=pp.session;delay=clamp_delay(pp.payload[8]);
        delay_auto=pp.payload[9]!=0;rollback_mode=pp.payload[10]!=0;rollback_window=rollback_mode?clamp_rollback(pp.payload[11]):0;rollback_mode=rollback_window!=0;rtt_ms=get16(pp.payload+12);jitter_ms=get16(pp.payload+14);s_ruleset=(uint8_t)(pp.payload[16]&3u);{unsigned n=pp.payload[17];if(n>R498_PROFILE_NAME_MAX)n=R498_PROFILE_NAME_MAX;if((int)(18+n)<=pp.payload_len){memcpy(s_peer_profile,pp.payload+18,n);s_peer_profile[n]=0;}}offered=true;
        ++samples;s_stats.rtt_ms=rtt_ms;s_stats.jitter_ms=jitter_ms;s_stats.calibration_samples=samples;
        s_stats.auto_delay=(uint8_t)delay;s_stats.delay_auto=(uint8_t)(delay_auto?1:0);
        Xbox_Log("ZNET R49 OFFER session=%08lX base=%d rb=%u netcode=%s mode=%s rtt=%lums jitter=%lums\n",
                 (unsigned long)s_session,delay,(unsigned)rollback_window,rollback_mode?"ROLLBACK":"DELAY",delay_auto?"AUTO":"MANUAL",
                 (unsigned long)rtt_ms,(unsigned long)jitter_ms);send_peer(ZNP_READY,0,0);last_ready=now;
      }else if(offered&&pp.session==s_session&&pp.type==ZNP_PING&&pp.payload_len>=8){
        send_peer(ZNP_PONG,pp.payload,8);
      }else if(offered&&pp.session==s_session&&pp.type==ZNP_START&&pp.payload_len>=7){
        delay=clamp_delay(pp.payload[0]);rollback_mode=pp.payload[4]!=0;rollback_window=rollback_mode?clamp_rollback(pp.payload[5]):0;rollback_mode=rollback_window!=0;if(pp.payload[1]!=2||pp.payload[2]!=0||pp.payload[3]!=1||(pp.payload[6]&3u)!=s_ruleset)return false;if(!started) {
          started=true;start_tick=GetTickCount();
          Xbox_Log("ZNET R49.6 guest START received - preparing\n");public_diag_flush();
          prepare_progress=s_prepare?s_prepare(true):100;if(prepare_progress<0)return false;
        }
        send_peer(ZNP_START_ACK,0,0);
        if(prepare_progress==100)send_peer(ZNP_BOOT_READY,0,0);
      }else if(started&&prepare_progress==100&&pp.session==s_session&&pp.type==ZNP_BOOT_GO){
        if(pp.payload_len<4 || (pp.payload[3]&3u)!=s_ruleset)return false;
        delay=clamp_delay(pp.payload[0]);rollback_window=pp.payload[1]?clamp_rollback(pp.payload[2]):0;rollback_mode=rollback_window!=0;
        znp_core_init(&s_core,1,(uint8_t)delay);s_stats.delay=(uint8_t)delay;s_stats.local_slot=1;s_stats.active=1;
        s_rollback_enabled=rollback_mode;s_rollback_window=rollback_window;rollback_reset((uint8_t)delay,rollback_window);
        Xbox_Log("ZNET R49 START join slot=P2 host=P1 base=%d rb=%u netcode=%s mode=%s rtt=%lums jitter=%lums\n",
                 delay,(unsigned)rollback_window,rollback_mode?"ROLLBACK":"DELAY",delay_auto?"AUTO":"MANUAL",(unsigned long)rtt_ms,(unsigned long)jitter_ms);return true;
      }
    }
    } // drain lobby packets
    if(started && prepare_progress<100) {
      prepare_progress=s_prepare?s_prepare(false):100;if(prepare_progress<0)return false;
    }
    if(started && prepare_progress==100 && GetTickCount()-last_ready>=100) {
      send_peer(ZNP_START_ACK,0,0);send_peer(ZNP_BOOT_READY,0,0);last_ready=GetTickCount();
    }
    char targetline[64],route[64],status[64],delayline[96];if(public_join)snprintf(targetline,sizeof(targetline),"PUBLIC ROOM %s",s_public_room);else snprintf(targetline,sizeof(targetline),"HOST %s",hostip);
    if(public_join)snprintf(route,sizeof(route),"ROUTE %s",s_public_direct?(s_public_lan_candidate?"LAN DIRECT":"WAN DIRECT"):(s_public_relay_rx?"SERVER RELAY":"AUTO NAT / RELAY"));else strcpy(route,"UDP 6464");
    if(delay_auto)snprintf(delayline,sizeof(delayline),"%s\nAUTO %dF RTT %luMS\nROLLBACK: %s",r498_ruleset_name(s_ruleset),delay,(unsigned long)rtt_ms,rollback_mode?"ENABLED":"DISABLED");else snprintf(delayline,sizeof(delayline),"%s\nMANUAL %dF\nROLLBACK: %s",r498_ruleset_name(s_ruleset),delay,rollback_mode?"ENABLED":"DISABLED");
    if(!offered)strcpy(status,"CONNECTING TO HOST");else if(!started)snprintf(status,sizeof(status),"READY - HOST %.12s STARTS",Xbox_Netplay_PeerProfileName());else if(prepare_progress<100)snprintf(status,sizeof(status),"PREPARING GAME %d%%",prepare_progress);else strcpy(status,"WAITING FOR HOST TO FINISH PREPARING");
    menu_frame(public_join?"JOIN PUBLIC ROOM":"JOIN DIRECT",targetline,route,status,delayline,public_join?"Y CHAT   B CANCEL":"B CANCEL");
    uint16_t mp=menu_pressed();if(mp&ZMENU_B){Xbox_Log("ZNET R49 JOIN cancelled\n");return false;}if(public_join&&(mp&ZMENU_Y)){r499_chat_menu(true);menu_latch_current();continue;}
    if((started && GetTickCount()-start_tick>120000) || (!offered && now-start_tick>60000)){menu_frame("JOIN DIRECT","CONNECTION TIMED OUT","CHECK HOST IP / NETWORK","","","B BACK");Sleep(1000);return false;}
    menu_yield();
  }
}

static bool is_lan_direct_1f(void) {
  // The 1F phase pacer exists for strict delay/lockstep.  In rollback mode it
  // only adds deliberate sleep in front of an input path that can already
  // absorb phase differences through prediction.
  return s_stats.active && !s_rollback_enabled && s_public_mode && s_public_direct &&
         s_public_lan_candidate && s_core.delay <= 1;
}

static uint32_t resend_interval_ms(void) {
  if (s_public_mode && s_public_direct && s_public_lan_candidate)
    return ZNP_RESEND_LAN_MS;
  uint32_t rtt = s_stats.rtt_ms;
  if (rtt && rtt <= 40u) return ZNP_RESEND_NEAR_MS;
  if (rtt && rtt <= 90u) return ZNP_RESEND_WAN_MS;
  return ZNP_RESEND_SLOW_MS;
}

static void phase_pace_observe(uint32_t waited_ms) {
  if (!is_lan_direct_1f()) {
    s_phase_wait_ewma_x8 = 0;
    s_phase_pace_ms = 0;
    return;
  }
  // Ignore long stalls/loss events; pacing should follow only the recurring
  // 1-8 ms phase offset seen with a faster physical Xbox vs Xemu.  Add the
  // pace already applied back into the sample, otherwise a successful pace
  // would make its own EWMA collapse and oscillate every few frames.
  uint32_t sample = waited_ms <= 8u ? waited_ms + s_phase_pace_ms : s_phase_pace_ms;
  uint32_t sx8 = sample * 8u;
  if (!s_phase_wait_ewma_x8) s_phase_wait_ewma_x8 = sx8;
  else s_phase_wait_ewma_x8 = (s_phase_wait_ewma_x8 * 7u + sx8) / 8u;
  uint32_t ewma_ms = (s_phase_wait_ewma_x8 + 4u) / 8u;
  uint32_t target = ewma_ms > 1u ? ewma_ms - 1u : 0u;
  if (target > 3u) target = 3u;
  // Move one millisecond at a time to avoid oscillating the two peers.
  if (s_phase_pace_ms < target) ++s_phase_pace_ms;
  else if (s_phase_pace_ms > target) --s_phase_pace_ms;
}

static void rollback_reset(uint8_t delay, uint8_t window) {
  memset(s_rb_pred, 0, sizeof(s_rb_pred));
  s_oldest_prediction = 0xffffffffu;
  s_rb_request = 0xffffffffu;
  s_rollback_window = window > ZNP_ROLLBACK_MAX ? ZNP_ROLLBACK_MAX : window;
  s_latest_actual_peer = delay ? (uint32_t)(delay - 1u) : 0xffffffffu;
  s_last_actual_peer_pad = 0;
}

static uint32_t rollback_actual_gap(uint32_t frame) {
  if (s_latest_actual_peer == 0xffffffffu) return frame + 1u;
  // A newer frame can arrive while this exact frame is still missing from a
  // redundant history packet.  Unsigned subtraction used to underflow here
  // and turn that harmless hole into a huge apparent lag spike.
  if (s_latest_actual_peer >= frame) return 0u;
  return frame - s_latest_actual_peer;
}

// Protect the oldest unresolved prediction even when newer authoritative
// packets arrive out of order. Latest-peer alone cannot bound rewind depth.
static bool rollback_can_advance(uint32_t frame) {
  if (s_oldest_prediction != 0xffffffffu && frame >= s_oldest_prediction &&
      frame - s_oldest_prediction >= s_rollback_window) return false;
  return rollback_actual_gap(frame) <= s_rollback_window;
}

static bool rollback_mark_prediction(uint32_t frame, uint16_t pad) {
  RollbackPrediction* p = &s_rb_pred[frame & (ZNP_HISTORY - 1)];
  if (p->valid && p->frame == frame) return true;
  if (p->valid || !rollback_can_advance(frame)) {
    s_core.fault = 1; s_core.fault_frame = frame; return false;
  }
  if (!znp_core_put_input(&s_core, (uint8_t)(1 - s_core.local_slot), frame, pad)) return false;
  p->frame = frame; p->valid = 1;
  if (s_oldest_prediction == 0xffffffffu) s_oldest_prediction = frame;
  ++s_stats.rollback_predictions;
  return true;
}

static bool rollback_accept_remote(uint8_t slot, uint32_t frame, uint16_t pad) {
  RollbackPrediction* pred = &s_rb_pred[frame & (ZNP_HISTORY - 1)];
  if (pred->valid && pred->frame == frame) {
    bool changed = false;
    if (!znp_core_correct_input(&s_core, slot, frame, pad, &changed)) return false;
    pred->valid = 0;
    if (s_oldest_prediction == frame) {
      s_oldest_prediction = 0xffffffffu;
      for (uint32_t f = frame + 1u; f <= s_core.frame; ++f) {
        const RollbackPrediction* next = &s_rb_pred[f & (ZNP_HISTORY - 1)];
        if (next->valid && next->frame == f) { s_oldest_prediction = f; break; }
      }
    }
    if (changed) {
      ++s_stats.rollback_corrections;
      if (s_stats.rollback_corrections <= 8u) {
        const uint32_t depth = s_core.frame > frame ? s_core.frame - frame : 0u;
        Xbox_Log("ZNET R49 correction frame=%lu current=%lu depth=%lu window=%u\n",
                 (unsigned long)frame, (unsigned long)s_core.frame,
                 (unsigned long)depth, (unsigned)s_rollback_window);
      }
      // If this frame is already outside the retained rollback window, fail
      // rather than silently continuing from an uncorrectable prediction.
      if (s_core.frame > frame && s_core.frame - frame > s_rollback_window) {
        Xbox_Log("ZNET R49 rollback correction too old frame=%lu current=%lu depth=%lu window=%u\n",
                 (unsigned long)frame, (unsigned long)s_core.frame,
                 (unsigned long)(s_core.frame - frame), (unsigned)s_rollback_window);
        s_core.fault = 1; s_core.fault_frame = frame;
        s_exit_reason = ZNP_EXIT_CORE; s_exit_reason_frame = frame;
        return false;
      }
      if (s_rb_request == 0xffffffffu || frame < s_rb_request) s_rb_request = frame;
    }
  } else if (!znp_core_put_input(&s_core, slot, frame, pad)) {
    return false;
  }
  if (s_latest_actual_peer == 0xffffffffu || frame >= s_latest_actual_peer) {
    s_latest_actual_peer = frame;
    s_last_actual_peer_pad = pad;
  }
  return true;
}

static const char* exit_reason_name(uint8_t reason) {
  switch (reason) {
    case ZNP_EXIT_PREPARE: return "PREPARE";
    case ZNP_EXIT_BEGIN: return "BEGIN";
    case ZNP_EXIT_SNAPSHOT_INIT: return "SNAPSHOT_INIT";
    case ZNP_EXIT_SNAPSHOT_LOAD: return "SNAPSHOT_LOAD";
    case ZNP_EXIT_REPLAY_INPUT: return "REPLAY_INPUT";
    case ZNP_EXIT_REPLAY_RUNTIME: return "REPLAY_RUNTIME";
    case ZNP_EXIT_SNAPSHOT_SAVE: return "SNAPSHOT_SAVE";
    case ZNP_EXIT_AFTERFRAME: return "AFTERFRAME";
    case ZNP_EXIT_RUNTIME: return "RUNTIME";
    case ZNP_EXIT_CORE: return "CORE";
    default: return "NORMAL";
  }
}

static void receive_inputs(void) {
  uint8_t buf[ZNP_MAX_PACKET];
  for (;;) {
    ParsedPacket pp; sockaddr_in from; memset(&from, 0, sizeof(from));
    int rr = recv_one(&pp, &from, buf);
    if (rr <= 0) break;
    if (!s_peer_known || !peer_equal(from, s_peer) || pp.session != s_session) continue;
    if (pp.type == ZNP_GOODBYE) {
      uint8_t reason = 0, peer_core_fault = 0;
      uint32_t peer_frame = 0xffffffffu;
      if (pp.payload_len >= 8) {
        reason = pp.payload[0];
        peer_core_fault = pp.payload[1];
        peer_frame = get32(pp.payload + 4);
      }
      s_core.fault = 1; s_core.fault_frame = s_core.frame;
      Xbox_Log("ZNET R49 peer left local-frame=%lu reason=%s(%u) peer-frame=%lu peer-core-fault=%u\n",
               (unsigned long)s_core.frame, exit_reason_name(reason), (unsigned)reason,
               (unsigned long)peer_frame, (unsigned)peer_core_fault);
      continue;
    }
    if (s_mode==ZNP_HOST_MODE && pp.type==ZNP_BOOT_READY) {
      uint8_t go[4]={s_core.delay,(uint8_t)(s_rollback_enabled?1:0),s_rollback_window,s_ruleset};
      send_peer(ZNP_BOOT_GO,go,4);continue;
    }
    if (pp.type != ZNP_INPUT || pp.payload_len < 24) continue;
    uint8_t slot = pp.payload[0], count = pp.payload[1];
    uint8_t remote = (uint8_t)(1 - s_core.local_slot);
    if (slot != remote || count == 0 || count > ZNP_SEND_HISTORY) continue;
    uint32_t first = get32(pp.payload + 4);
    uint32_t consumed = get32(pp.payload + 8);
    uint32_t hash_frame = get32(pp.payload + 12);
    uint64_t hash = get64(pp.payload + 16);
    if (pp.payload_len != 24 + count * 2) continue;
    for (uint8_t i = 0; i < count; ++i) {
      uint16_t pad = get16(pp.payload + 24 + i * 2);
      const uint32_t input_frame = first + i;
      if (s_rollback_enabled) {
        if (!rollback_accept_remote(slot, input_frame, pad)) break;
      } else if (!znp_core_put_input(&s_core, slot, input_frame, pad)) break;
    }
    s_stats.peer_consumed_frame = consumed;
    if (hash_frame != 0xffffffffu) {
      ZnpHashSlot* local = &s_core.local_hash[hash_frame & (ZNP_HISTORY - 1)];
      if (local->valid && local->frame == hash_frame && local->hash == hash && s_last_hash_seen != hash_frame) {
        ++s_stats.hash_matches; s_last_hash_seen = hash_frame;
      }
      znp_core_put_peer_hash(&s_core, hash_frame, hash);
    }
  }
}

static bool send_input_history(uint32_t newest) {
  uint8_t payload[24 + ZNP_SEND_HISTORY * 2];
  uint32_t first = newest >= (ZNP_SEND_HISTORY - 1) ? newest - (ZNP_SEND_HISTORY - 1) : 0;
  // `peer_consumed_frame` is the peer's next simulation frame. Inputs before
  // that frame are already consumed there, so do not keep retransmitting them.
  // In delay mode peer_consumed means the peer definitely had this input.
  // In rollback mode the peer may have *predicted* consumed frames, so it is
  // not an acknowledgement of authoritative delivery. Keep the normal 32-frame
  // redundant history there until we add an explicit authoritative ACK.
  if (!s_rollback_enabled && s_stats.peer_consumed_frame > first && s_stats.peer_consumed_frame <= newest)
    first = s_stats.peer_consumed_frame;
  // Do not advertise holes from before the session's delayed neutral prefix.
  while (first < newest && !znp_core_get_input(&s_core, s_core.local_slot, first, 0)) ++first;
  uint8_t count = (uint8_t)(newest - first + 1);
  payload[0] = s_core.local_slot; payload[1] = count; put16(payload + 2, 0);
  put32(payload + 4, first); put32(payload + 8, s_core.frame);
  uint32_t hf = 0xffffffffu; uint64_t hv = 0;
  if (znp_core_latest_local_hash(&s_core, &hf, &hv) && (hf % ZNP_HASH_PERIOD) != ZNP_HASH_PERIOD - 1) {
    hf = 0xffffffffu; hv = 0;
  }
  put32(payload + 12, hf); put64(payload + 16, hv);
  for (uint8_t i = 0; i < count; ++i) {
    uint16_t pad = 0;
    if (!znp_core_get_input(&s_core, s_core.local_slot, first + i, &pad)) return false;
    put16(payload + 24 + i * 2, pad);
  }
  return send_peer(ZNP_INPUT, payload, 24 + count * 2);
}

static void close_transport(void){
  if(s_sock!=INVALID_SOCKET&&s_public_mode)public_leave();
  if(s_sock!=INVALID_SOCKET){closesocket(s_sock);s_sock=INVALID_SOCKET;}
  // R49.9.3: keep XNet/Winsock alive across menu transitions. Reinitializing
  // them repeatedly caused multi-second stalls. The process owns final cleanup.
  s_peer_known=false;s_session=0;s_stats.active=0;s_rollback_enabled=false;s_rollback_window=0;
  s_exit_reason=0;s_exit_reason_frame=0;rollback_reset(0,0);public_reset();
}

bool Xbox_Netplay_Init(uint64_t rom_hash) {
  s_rom_hash=rom_hash;s_mode=ZNP_OFF;s_r499_session_auth=false;s_r499_session_auth_name[0]=0;
  s_local_ip_cached=false;s_public_ip_cached=false;s_public_ip_cached_tick=0;
  memset(&s_board_cache,0,sizeof(s_board_cache));memset(&s_board_async,0,sizeof(s_board_async));
  memset(&s_score_upload_async,0,sizeof(s_score_upload_async));
  return true;
}
bool Xbox_Netplay_Open(int choice) {
  close_transport();
  memset(&s_stats,0,sizeof(s_stats));memset(&s_peer,0,sizeof(s_peer));s_peer_known=false;s_seq=1;s_last_hash_seen=0xffffffffu;s_phase_wait_ewma_x8=0;s_phase_pace_ms=0;s_phase_last_frame_tick=0;s_prepared_ready=false;s_prepared_frame=0;s_prepared_pads[0]=s_prepared_pads[1]=0;s_rollback_enabled=false;s_rollback_window=0;s_exit_reason=0;s_exit_reason_frame=0;rollback_reset(0,0);public_reset();
  memset(&s_core,0,sizeof(s_core)); s_last_rx=s_last_tx=0;strcpy(s_peer_profile,"PLAYER");
  if(choice<1 || choice>3)return false;
  sockaddr_in target;memset(&target,0,sizeof(target));
  if(choice==1){int rm=r498_ruleset_menu("HOST DIRECT MODE",false);if(rm<0)return false;s_ruleset=(uint8_t)rm;s_mode=ZNP_HOST_MODE;if(!open_socket(false)){menu_frame("HOST DIRECT","NETWORK INIT FAILED","CHECK ETHERNET / RXDK NETWORK","","","B BACK");Sleep(1200);close_transport();return false;}if(wait_host_handshake(false))return true;close_transport();return false;}
  if(choice==2){s_mode=ZNP_JOIN_MODE;if(!open_socket(false)){menu_frame("JOIN DIRECT","NETWORK INIT FAILED","CHECK ETHERNET / RXDK NETWORK","","","B BACK");Sleep(1200);close_transport();return false;}if(!direct_ip_editor("JOIN DIRECT","ENTER HOST IPV4",&target)){close_transport();return false;}if(wait_join_handshake(target,false))return true;close_transport();return false;}
  int pm=public_mode_menu();if(pm<0)return false;if(pm==2){if(!ensure_network_stack()){menu_frame("WORLD CHAT","NETWORK INIT FAILED","CHECK INTERNET / UDP 6467","","","B BACK");Sleep(900);return false;}r499_chat_menu(false);return false;}
  if(pm==0){int rm=r498_ruleset_menu("PUBLIC ROOM MODE",false);if(rm<0)return false;s_ruleset=(uint8_t)rm;}
  s_public_mode=true;s_public_host=(pm==0);s_mode=s_public_host?ZNP_HOST_MODE:ZNP_JOIN_MODE;if(!open_socket(true)){menu_frame("PUBLIC ROOMS","NETWORK INIT FAILED","CHECK ETHERNET / RXDK NETWORK","","","B BACK");Sleep(1200);close_transport();return false;}
  if(s_public_host){if(!public_register_host()){menu_frame("HOST PUBLIC ROOM","SERVER UNAVAILABLE","CHECK INTERNET / UDP 6467","","","B BACK");Sleep(1200);close_transport();return false;}if(wait_host_handshake(true))return true;close_transport();return false;}
  if(!public_browse_rooms(&target)){close_transport();return false;}if(wait_join_handshake(target,true))return true;close_transport();
  return false;
}

void Xbox_Netplay_Shutdown(void) {
  if (s_stats.active && s_peer_known) {
    uint8_t bye[8] = {0};
    bye[0] = s_exit_reason;
    bye[1] = (uint8_t)(s_core.fault ? 1u : 0u);
    put32(bye + 4, s_exit_reason ? s_exit_reason_frame : s_core.frame);
    for (int i = 0; i < 2; ++i) send_peer(ZNP_GOODBYE, bye, sizeof(bye));
  }
  close_transport();
  s_mode = ZNP_OFF;
}

void Xbox_Netplay_ReportFatal(uint8_t reason, uint32_t frame) {
  if (!reason) reason = ZNP_EXIT_CORE;
  if (!s_exit_reason) {
    s_exit_reason = reason;
    s_exit_reason_frame = frame;
  }
  Xbox_Log("ZNET R49 local fatal reason=%s(%u) frame=%lu core-fault=%u core-frame=%lu\n",
           exit_reason_name(reason), (unsigned)reason, (unsigned long)frame,
           (unsigned)(s_core.fault ? 1u : 0u), (unsigned long)s_core.fault_frame);
}

bool Xbox_Netplay_Active(void) { return s_stats.active != 0; }
const char* Xbox_Netplay_Role(void) {
  return s_mode == ZNP_HOST_MODE ? "HOST" : s_mode == ZNP_JOIN_MODE ? "JOIN" : "OFF";
}

void Xbox_Netplay_PaceBeforeInput(void) {
  const DWORD now = GetTickCount();
  uint32_t requested = is_lan_direct_1f() ? s_phase_pace_ms : 0u;
  uint32_t applied = 0;

  if (requested && s_phase_last_frame_tick) {
    const uint32_t elapsed = (uint32_t)(now - s_phase_last_frame_tick);
    // Only consume genuine spare time.  If the previous start-to-start period
    // is already at/over 16 ms, adding a sleep would lower FPS rather than
    // replace an otherwise-idle phase gap.
    if (elapsed < ZNP_PACE_BUDGET_MS) {
      const uint32_t slack = ZNP_PACE_BUDGET_MS - elapsed;
      applied = requested < slack ? requested : slack;
    }
    if (applied < requested) ++s_stats.pace_budget_skips;
  }

  s_phase_last_frame_tick = now;
  s_stats.pace_ms = (uint8_t)applied;
  if (!applied) return;

  Sleep((DWORD)applied);
  s_stats.pace_ms_total += applied;
  // Drain anything that arrived while using otherwise-free frame slack so
  // BeginFrame sees it immediately after the local pad is sampled.
  receive_inputs();
}

void Xbox_Netplay_MidFrame(uint32_t frame) {
  if (!s_stats.active || s_core.fault || frame != s_core.frame) return;
  if (s_public_mode) public_tick();
  // The first copy went out at BeginFrame.  Put the redundant copy here,
  // immediately after simulation and before video/audio/present, instead of
  // waiting until AfterFrame.  Skip it if the peer is already beyond `frame`.
  if ((s_rollback_enabled || s_stats.peer_consumed_frame <= frame) &&
      send_input_history(s_core.latest_local))
    ++s_stats.proactive_copies;
  receive_inputs();
}

bool Xbox_Netplay_PrepareFrame(uint32_t frame) {
  if (!s_stats.active) return false;
  if (s_public_mode) public_tick();
  if (s_core.fault) {
    Xbox_Log("ZNET R49 FAULT/PREPARE requested=%lu core-frame=%lu fault-frame=%lu localHash=%08lX%08lX peerHash=%08lX%08lX\n",
             (unsigned long)frame, (unsigned long)s_core.frame,
             (unsigned long)s_core.fault_frame,
             (unsigned long)(s_core.fault_local_hash >> 32), (unsigned long)s_core.fault_local_hash,
             (unsigned long)(s_core.fault_peer_hash >> 32), (unsigned long)s_core.fault_peer_hash);
    s_stats.fault = 1; s_stats.fault_frame = s_core.fault_frame;
    return false;
  }
  if (frame != s_core.frame) {
    Xbox_Log("ZNET R49 FRAME/PREPARE mismatch requested=%lu core-frame=%lu\n",
             (unsigned long)frame, (unsigned long)s_core.frame);
    return false;
  }

  s_prepared_ready = false;
  s_prepared_frame = frame;

  // R49: rollback may still need to stall if the peer has fallen farther
  // behind than the retained rewind window.  Do that BEFORE Xbox_Input_Poll so
  // a 40-80ms catch-up event does not age a freshly sampled button press.
  if (s_rollback_enabled) {
    DWORD start = GetTickCount(), last_send = start;
    bool waited = false;
    for (;;) {
      receive_inputs();
      if (s_core.fault) {
        s_stats.fault = 1; s_stats.fault_frame = s_core.fault_frame;
        return false;
      }
      if (rollback_can_advance(frame)) {
        const DWORD waited_ms = GetTickCount() - start;
        if (waited || waited_ms) {
          ++s_stats.wait_frames;
          s_stats.wait_ms += (uint32_t)waited_ms;
          s_stats.preinput_wait_ms += (uint32_t)waited_ms;
          ++s_stats.preinput_wait_frames;
          if ((uint32_t)waited_ms > s_stats.max_wait_ms) s_stats.max_wait_ms = (uint32_t)waited_ms;
        }
        // Rollback does not use the strict-lockstep phase pacer.
        s_stats.pace_ms = 0;
        return true;
      }
      const DWORD now = GetTickCount();
      waited = true;
      ++s_stats.wait_loops;
      if (now - last_send >= resend_interval_ms()) {
        if (send_input_history(s_core.latest_local)) ++s_stats.input_resends;
        last_send = now;
      }
      if (now - s_last_rx > ZNP_TIMEOUT_MS && now - start > ZNP_TIMEOUT_MS) {
        Xbox_Log("ZNET R49 timeout rollback-preinput frame=%lu peer-actual=%lu window=%u\n",
                 (unsigned long)frame, (unsigned long)s_latest_actual_peer,
                 (unsigned)s_rollback_window);
        s_stats.fault = 1; s_stats.fault_frame = frame; return false;
      }
      Sleep(1);
    }
  }

  // Delay 0 needs the just-sampled local input for this same frame, so it must
  // keep the old BeginFrame wait path. Any delayed mode already has the local
  // frame input in history and can wait for the peer before sampling the next
  // future local input.
  if (s_core.delay == 0) return true;

  DWORD start = GetTickCount(), last_send = start;
  bool waited = false;
  for (;;) {
    receive_inputs();
    if (s_core.fault) {
      s_stats.fault = 1; s_stats.fault_frame = s_core.fault_frame;
      return false;
    }
    if (znp_core_frame_ready(&s_core, frame, s_prepared_pads)) {
      const DWORD waited_ms = GetTickCount() - start;
      if (waited || waited_ms) {
        ++s_stats.wait_frames;
        s_stats.wait_ms += (uint32_t)waited_ms;
        s_stats.preinput_wait_ms += (uint32_t)waited_ms;
        ++s_stats.preinput_wait_frames;
        if ((uint32_t)waited_ms > s_stats.max_wait_ms) s_stats.max_wait_ms = (uint32_t)waited_ms;
      }
      phase_pace_observe((uint32_t)waited_ms);
      s_stats.pace_ms = s_phase_pace_ms;
      s_prepared_ready = true;
      return true;
    }
    const DWORD now = GetTickCount();
    waited = true;
    ++s_stats.wait_loops;
    if (now - last_send >= resend_interval_ms()) {
      // We are waiting for the peer's current frame, but our future local input
      // was sent on earlier frames. A history copy can still repair loss without
      // sampling the next controller state early.
      if (s_rollback_enabled || s_stats.peer_consumed_frame <= frame) {
        if (send_input_history(s_core.latest_local)) ++s_stats.input_resends;
      } else {
        ++s_stats.resend_suppressed;
      }
      last_send = now;
    }
    if (now - s_last_rx > ZNP_TIMEOUT_MS && now - start > ZNP_TIMEOUT_MS) {
      Xbox_Log("ZNET R49 timeout preinput frame=%lu peer-latest=%lu peer-consumed=%lu\n",
               (unsigned long)frame, (unsigned long)s_core.latest_peer,
               (unsigned long)s_stats.peer_consumed_frame);
      s_stats.fault = 1; s_stats.fault_frame = frame; return false;
    }
    Sleep(1);
  }
}

bool Xbox_Netplay_BeginFrame(uint32_t frame, uint16_t local_pad,
                             uint16_t* p1, uint16_t* p2) {
  if (!s_stats.active) return false;
  if(s_public_mode)public_tick();
  if (s_core.fault || frame != s_core.frame) return false;
  uint32_t target = frame + s_core.delay;
  if (!znp_core_put_input(&s_core, s_core.local_slot, target, local_pad)) return false;
  // Drain first so a queued peer input can satisfy this frame before we spend
  // time on transmission; then publish our newly sampled future input.
  receive_inputs();
  send_input_history(target);

  if (s_rollback_enabled) {
    const uint8_t remote = (uint8_t)(1 - s_core.local_slot);
    if (!znp_core_get_input(&s_core, remote, frame, 0)) {
      if (rollback_can_advance(frame)) {
        if (!rollback_mark_prediction(frame, s_last_actual_peer_pad)) return false;
      }
    }
    uint16_t rpads[2];
    if (znp_core_frame_ready(&s_core, frame, rpads)) {
      if (p1) *p1 = rpads[0]; if (p2) *p2 = rpads[1];
      return true;
    }
    // Beyond the rollback window, fall through to the normal bounded wait.
  }

  // R48: PrepareFrame already waited for the current remote input before
  // the physical pad poll. Return that cached pair immediately so the freshly
  // sampled local input is not aged by network waiting.
  if (s_core.delay > 0 && s_prepared_ready && s_prepared_frame == frame) {
    if (p1) *p1 = s_prepared_pads[0];
    if (p2) *p2 = s_prepared_pads[1];
    s_prepared_ready = false;
    ++s_stats.late_sample_frames;
    return true;
  }

  DWORD start = GetTickCount(), last_send = start;
  bool waited = false;
  uint16_t pads[2];
  for (;;) {
    receive_inputs();
    if (s_core.fault) {
      Xbox_Log("ZNET R49 FAULT frame=%lu localHash=%08lX%08lX peerHash=%08lX%08lX\n",
               (unsigned long)s_core.fault_frame,
               (unsigned long)(s_core.fault_local_hash >> 32), (unsigned long)s_core.fault_local_hash,
               (unsigned long)(s_core.fault_peer_hash >> 32), (unsigned long)s_core.fault_peer_hash);
      s_stats.fault = 1; s_stats.fault_frame = s_core.fault_frame;
      return false;
    }
    if (znp_core_frame_ready(&s_core, frame, pads)) {
      DWORD waited_ms = GetTickCount() - start;
      if (waited || waited_ms) {
        ++s_stats.wait_frames;
        s_stats.wait_ms += (uint32_t)waited_ms;
        if ((uint32_t)waited_ms > s_stats.max_wait_ms) s_stats.max_wait_ms = (uint32_t)waited_ms;
      }
      phase_pace_observe((uint32_t)waited_ms);
      s_stats.pace_ms = s_phase_pace_ms;
      if (p1) *p1 = pads[0]; if (p2) *p2 = pads[1];
      return true;
    }
    DWORD now = GetTickCount();
    waited = true;
    ++s_stats.wait_loops;
    if (now - last_send >= resend_interval_ms()) {
      // If the peer already advanced beyond this frame it has consumed our
      // current input; another resend cannot unblock it.
      if (s_rollback_enabled || s_stats.peer_consumed_frame <= frame) {
        if (send_input_history(target)) ++s_stats.input_resends;
      } else {
        ++s_stats.resend_suppressed;
      }
      last_send = now;
    }
    if (now - s_last_rx > ZNP_TIMEOUT_MS && now - start > ZNP_TIMEOUT_MS) {
      Xbox_Log("ZNET R49 timeout waiting frame=%lu target=%lu peer-latest=%lu peer-consumed=%lu\n",
               (unsigned long)frame, (unsigned long)target,
               (unsigned long)s_core.latest_peer,
               (unsigned long)s_stats.peer_consumed_frame);
      s_stats.fault = 1; s_stats.fault_frame = frame; return false;
    }
    Sleep(1);
  }
}

bool Xbox_Netplay_RollbackEnabled(void) { return s_rollback_enabled; }
uint8_t Xbox_Netplay_RollbackWindow(void) { return s_rollback_window; }

bool Xbox_Netplay_TakeRollbackRequest(uint32_t current_frame, uint32_t* from_frame) {
  if (!s_rollback_enabled || s_rb_request == 0xffffffffu || s_rb_request >= current_frame)
    return false;
  const uint32_t f = s_rb_request;
  s_rb_request = 0xffffffffu;
  if (current_frame - f > s_rollback_window) {
    Xbox_Log("ZNET R49 rollback request too old frame=%lu current=%lu depth=%lu window=%u\n",
             (unsigned long)f, (unsigned long)current_frame,
             (unsigned long)(current_frame - f), (unsigned)s_rollback_window);
    s_core.fault = 1; s_core.fault_frame = f;
    s_stats.fault = 1; s_stats.fault_frame = f;
    s_exit_reason = ZNP_EXIT_CORE; s_exit_reason_frame = f;
    return false;
  }
  if (from_frame) *from_frame = f;
  ++s_stats.rollback_events;
  return true;
}

bool Xbox_Netplay_GetReplayInputs(uint32_t frame, uint16_t* p1, uint16_t* p2) {
  uint16_t pads[2];
  if (!s_rollback_enabled || !znp_core_frame_ready(&s_core, frame, pads)) return false;
  if (p1) *p1 = pads[0]; if (p2) *p2 = pads[1];
  return true;
}

void Xbox_Netplay_NoteReplay(uint32_t depth) {
  s_stats.rollback_replayed_frames += depth;
  if (depth > s_stats.rollback_max_depth) s_stats.rollback_max_depth = depth;
}

// Exact input status only. Earlier speculation does not require a snapshot
// on this frame: replay starts at the earlier predicted frame's saved state.
bool Xbox_Netplay_RemoteInputAuthoritative(uint32_t frame) {
  if (!s_stats.active) return false;
  if (!s_rollback_enabled) return true;
  const RollbackPrediction* pred = &s_rb_pred[frame & (ZNP_HISTORY - 1)];
  return znp_core_get_input(&s_core, (uint8_t)(1 - s_core.local_slot), frame, 0) &&
         !(pred->valid && pred->frame == frame);
}

bool Xbox_Netplay_RollbackHealthy(void) { return s_stats.active && !s_core.fault; }

bool Xbox_Netplay_FrameAuthoritative(uint32_t frame) {
  if (!s_stats.active) return false;
  if (!s_rollback_enabled) return true;
  // A hash depends on every earlier input, not just the sampled hash frame.
  if (s_oldest_prediction != 0xffffffffu && s_oldest_prediction <= frame) return false;

  const uint8_t remote = (uint8_t)(1 - s_core.local_slot);
  uint16_t pad = 0;
  if (!znp_core_get_input(&s_core, remote, frame, &pad)) return false;

  // A value can exist in the core because rollback predicted it. Hashes must
  // not become authoritative until the real remote input for that exact frame
  // has arrived. Otherwise a recoverable prediction can race hash comparison
  // and be misreported as a desync before rollback gets a chance to replay.
  const RollbackPrediction* pred = &s_rb_pred[frame & (ZNP_HISTORY - 1)];
  if (pred->valid && pred->frame == frame) return false;
  if (s_latest_actual_peer == 0xffffffffu || s_latest_actual_peer < frame) return false;

  // If a changed authoritative input has arrived but its replay has not yet
  // run, the stored hash still describes the predicted timeline. Wait until
  // the pending correction is consumed and R48_RecordHash has been refreshed.
  if (s_rb_request != 0xffffffffu && s_rb_request <= frame) return false;
  return true;
}

void Xbox_Netplay_SubmitConfirmedHash(uint32_t frame, uint64_t hash) {
  if (!s_stats.active || !hash) return;
  znp_core_put_local_hash(&s_core, frame, hash);
}

bool Xbox_Netplay_AfterFrame(uint32_t frame, uint64_t state_hash) {
  if (!s_stats.active || s_core.fault || frame != s_core.frame) return false;
  if(s_public_mode)public_tick();
  znp_core_commit_frame(&s_core, frame);
  if (state_hash) znp_core_put_local_hash(&s_core, frame, state_hash);
  s_stats.current_frame = s_core.frame;
  // R48 moved the redundant history copy to Xbox_Netplay_MidFrame(),
  // before video/audio/present.  Keep draining here for packets that arrived
  // during presentation and for prompt hash comparison.
  receive_inputs();
  if (s_core.fault) {
    Xbox_Log("ZNET R49 DESYNC frame=%lu local=%08lX%08lX peer=%08lX%08lX\n",
             (unsigned long)s_core.fault_frame,
             (unsigned long)(s_core.fault_local_hash >> 32), (unsigned long)s_core.fault_local_hash,
             (unsigned long)(s_core.fault_peer_hash >> 32), (unsigned long)s_core.fault_peer_hash);
    s_stats.fault = 1; s_stats.fault_frame = s_core.fault_frame;
    return false;
  }
  return true;
}

void Xbox_Netplay_GetStats(XboxNetplayStats* out) {
  if (!out) return;
  *out = s_stats;
  out->delay = s_core.delay; out->local_slot = s_core.local_slot;
  out->current_frame = s_core.frame; out->latest_peer_frame = s_core.latest_peer;
  out->fault = s_core.fault; out->fault_frame = s_core.fault_frame;
}

// ---------------------------------------------------------------------------
// R49.7 shared online pause/save control channel.
// ---------------------------------------------------------------------------
uint8_t Xbox_Netplay_LocalSlot(void) {
  return s_stats.active ? s_core.local_slot : 0xffu;
}

bool Xbox_Netplay_ControlSend(uint8_t kind, const void* data, int len) {
  if (!s_stats.active || !s_peer_known || len < 0 || len > (ZNP_MAX_PACKET - ZNP_HEADER - 1))
    return false;
  uint8_t payload[ZNP_MAX_PACKET - ZNP_HEADER];
  payload[0] = kind;
  if (len && data) memcpy(payload + 1, data, (size_t)len);
  return send_peer(ZNP_CONTROL, payload, len + 1);
}

int Xbox_Netplay_ControlRecv(uint8_t* kind, void* data, int cap, uint32_t timeout_ms) {
  if (!s_stats.active || !s_peer_known || !kind || cap < 0) return -1;
  const DWORD start = GetTickCount();
  uint8_t storage[ZNP_MAX_PACKET];
  for (;;) {
    if (s_public_mode) public_tick();
    ParsedPacket pp; sockaddr_in from; memset(&from, 0, sizeof(from));
    const int rr = recv_one(&pp, &from, storage);
    if (rr < 0) return -1;
    if (rr > 0) {
      if (!peer_equal(from, s_peer) || pp.session != s_session) continue;
      if (pp.type == ZNP_CONTROL && pp.payload_len >= 1) {
        *kind = pp.payload[0];
        const int n = pp.payload_len - 1;
        const int copy = n < cap ? n : cap;
        if (copy && data) memcpy(data, pp.payload + 1, (size_t)copy);
        return copy;
      }
      if (pp.type == ZNP_PING && pp.payload_len >= 8) {
        send_peer(ZNP_PONG, pp.payload, 8);
        continue;
      }
      if (pp.type == ZNP_GOODBYE) {
        s_core.fault = 1; s_core.fault_frame = s_core.frame;
        s_stats.fault = 1; s_stats.fault_frame = s_core.frame;
        return -1;
      }
      // Input packets already in flight when the shared pause begins can be
      // discarded here. The normal redundant history sender repairs any future
      // input hole once gameplay resumes.
    }
    if (timeout_ms == 0 || GetTickCount() - start >= timeout_ms) return 0;
    Sleep(1);
  }
}

bool Xbox_Netplay_ResyncAfterLoad(uint32_t frame) {
  if (!s_stats.active || !s_peer_known) return false;
  const uint8_t local = s_core.local_slot;
  const uint8_t delay = s_core.delay;
  const uint8_t window = s_rollback_window;
  znp_core_init(&s_core, local, delay);
  s_core.frame = frame;

  // Seed the delay pipeline with synchronized neutral input. This avoids
  // needing to serialize stale pre-load input history into save files.
  if (delay) {
    for (uint32_t f = frame; f < frame + delay; ++f) {
      if (!znp_core_put_input(&s_core, 0, f, 0) ||
          !znp_core_put_input(&s_core, 1, f, 0)) return false;
    }
  }

  rollback_reset(delay, window);
  if (delay) {
    s_latest_actual_peer = frame + delay - 1u;
    s_last_actual_peer_pad = 0;
  } else {
    s_latest_actual_peer = 0xffffffffu;
    s_last_actual_peer_pad = 0;
  }
  s_prepared_ready = false;
  s_prepared_frame = frame;
  s_prepared_pads[0] = s_prepared_pads[1] = 0;
  s_phase_wait_ewma_x8 = 0;
  s_phase_pace_ms = 0;
  s_phase_last_frame_tick = 0;
  s_last_hash_seen = 0xffffffffu;
  s_stats.current_frame = frame;
  s_stats.latest_peer_frame = delay ? frame + delay - 1u : frame;
  s_stats.peer_consumed_frame = frame;
  s_stats.fault = 0;
  s_stats.fault_frame = 0;
  return true;
}

