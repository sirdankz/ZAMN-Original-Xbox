#include "xbox_platform.h"
#ifdef ZAMN_R47_NETPLAY
#include "xbox_netplay.h"
#include "xbox_title_online.h"
#endif

extern "C" {
#include "native/runtime.h"
}

#if !defined(ZAMN_RELEASE_NO_DIAGNOSTICS) || defined(ZAMN_R499_SERVICE_LOG)
static FILE *s_log = NULL;
static char s_log_path[64] = {0};
#endif
#ifdef ZAMN_R39_BUFFERED_LOG
// R39: keep performance-window diagnostics in RAM during gameplay.  The old
// logger flushed every line to the Xbox filesystem and also mirrored every
// line through DbgPrint; the next 30-frame FPS window therefore included the
// previous window's logging stall.  One MiB comfortably holds several minutes
// of the current diagnostics and is flushed once at startup/end (or on fatal
// errors), not every 30 frames.
static char s_log_buffer[1024 * 1024];
#endif

// The native runtime renders the common 256-wide path into ordinary cached CPU
// RAM.  The Xbox texture is locked only after the game frame has completed.
static uint8_t s_native_stage[256 * 239 * 4] __attribute__((aligned(64)));
static uint8_t s_legacy_stage[ZAMN_FB_W * ZAMN_FB_H * 4]
    __attribute__((aligned(64)));

static bool s_exit_latched=false;
#ifdef ZAMN_R47_NETPLAY
static ZamnNativeRuntime* s_title_runtime;
static XboxTitleMenu s_title_menu;
static bool s_title_prepared=false;
static void Xbox_FlushLog(void);

// R49.5: the old online start replayed a neutral cold boot until the title
// coroutine reached its canonical yield (2224 frames on the supported USA
// ROM). Keep that exact state, but persist it once and restore it on later
// starts. The cache contains no ROM bytes.
#define R495_TITLE_CACHE_PATH "D:\\zamn-r49_5-title-cache.bin"
#define R495_TITLE_CACHE_MAGIC 0x3543535au /* 'ZSC5' */
#define R495_TITLE_CACHE_SCHEMA 1u
#define R495_TITLE_CACHE_BUILD 0x5A4D4905u
struct R495TitleCacheHeader {
  uint32_t magic, schema, build, snapshot_size;
  uint64_t rom_hash, state_hash;
  uint32_t audio_key, reserved;
};
static uint8_t* s_title_cache=NULL;
static int s_title_cache_size=0;
static uint64_t s_title_cache_hash=0;
static uint64_t s_title_rom_hash=0;

static uint32_t TitleAudioKey(void) {
  uint32_t h=2166136261u;
  const char* a=zamn_runtime_audio_mode_name(s_title_runtime);
  const char* b=zamn_runtime_audio_profile_name(s_title_runtime);
  for(const char* p=a; p && *p; ++p)h=(h^(uint8_t)*p)*16777619u;
  h=(h^0xffu)*16777619u;
  for(const char* p=b; p && *p; ++p)h=(h^(uint8_t)*p)*16777619u;
  return h;
}
static void TitleCacheDrop(void) {
  if(s_title_cache)free(s_title_cache);
  s_title_cache=NULL;s_title_cache_size=0;s_title_cache_hash=0;
}
static bool TitleCacheRead(void) {
  TitleCacheDrop();
  FILE* f=fopen(R495_TITLE_CACHE_PATH,"rb");
  if(!f){Xbox_Log("ZNET R49.5 title cache miss (first start will seed it)\n");return false;}
  R495TitleCacheHeader h;
  const int max_size=zamn_runtime_snapshot_size(s_title_runtime);
  const bool head=fread(&h,1,sizeof(h),f)==sizeof(h);
  if(!head || h.magic!=R495_TITLE_CACHE_MAGIC || h.schema!=R495_TITLE_CACHE_SCHEMA ||
     h.build!=R495_TITLE_CACHE_BUILD || h.rom_hash!=s_title_rom_hash ||
     h.audio_key!=TitleAudioKey() || h.snapshot_size==0 || max_size<=0 ||
     h.snapshot_size>(uint32_t)max_size) {
    fclose(f);Xbox_Log("ZNET R49.5 title cache stale/invalid - regenerate on start\n");return false;
  }
  uint8_t* data=(uint8_t*)malloc(h.snapshot_size);
  if(!data){fclose(f);Xbox_Log("ZNET R49.5 title cache allocation failed\n");return false;}
  const bool ok=fread(data,1,h.snapshot_size,f)==h.snapshot_size;fclose(f);
  if(!ok){free(data);Xbox_Log("ZNET R49.5 title cache truncated - regenerate on start\n");return false;}
  s_title_cache=data;s_title_cache_size=(int)h.snapshot_size;s_title_cache_hash=h.state_hash;
  Xbox_Log("ZNET R49.5 title cache loaded bytes=%d hash=%08lX%08lX\n",s_title_cache_size,
           (unsigned long)(s_title_cache_hash>>32),(unsigned long)s_title_cache_hash);
  return true;
}
static bool TitleCacheWriteCanonical(void) {
  const int cap=zamn_runtime_snapshot_size(s_title_runtime);
  if(cap<=0 || !zamn_runtime_title_ready(s_title_runtime))return false;
  uint8_t* data=(uint8_t*)malloc((size_t)cap);if(!data)return false;
  int used=0;
  if(!zamn_runtime_save_snapshot(s_title_runtime,data,cap,&used) || used<=0){free(data);return false;}
  const uint64_t hash=zamn_runtime_state_hash(s_title_runtime);
  R495TitleCacheHeader h={R495_TITLE_CACHE_MAGIC,R495_TITLE_CACHE_SCHEMA,R495_TITLE_CACHE_BUILD,
                          (uint32_t)used,s_title_rom_hash,hash,TitleAudioKey(),0};
  FILE* f=fopen(R495_TITLE_CACHE_PATH,"wb");
  bool disk=false;
  if(f){disk=fwrite(&h,1,sizeof(h),f)==sizeof(h) && fwrite(data,1,(size_t)used,f)==(size_t)used;fclose(f);}
  TitleCacheDrop();s_title_cache=data;s_title_cache_size=used;s_title_cache_hash=hash;
  Xbox_Log("ZNET R49.5 canonical title cache seeded bytes=%d hash=%08lX%08lX disk=%u\n",used,
           (unsigned long)(hash>>32),(unsigned long)hash,(unsigned)disk);
  return true;
}
static bool TitleCacheRestore(bool start) {
  if(!s_title_cache || s_title_cache_size<=0)return false;
  if(!zamn_runtime_load_snapshot(s_title_runtime,s_title_cache,s_title_cache_size) ||
     !zamn_runtime_title_ready(s_title_runtime)) {
    Xbox_Log("ZNET R49.5 cached title restore rejected - fallback to canonical boot\n");
    TitleCacheDrop();return false;
  }
  const uint64_t title_hash=zamn_runtime_state_hash(s_title_runtime);
  if(title_hash!=s_title_cache_hash){
    Xbox_Log("ZNET R49.5 cached title hash mismatch got=%08lX%08lX expected=%08lX%08lX\n",
      (unsigned long)(title_hash>>32),(unsigned long)title_hash,
      (unsigned long)(s_title_cache_hash>>32),(unsigned long)s_title_cache_hash);
    TitleCacheDrop();return false;
  }
  if(start && !zamn_runtime_title_start(s_title_runtime)){TitleCacheDrop();return false;}
  if(start){
    const uint64_t start_hash=zamn_runtime_state_hash(s_title_runtime);
    Xbox_Log("ZNET R49.5 instant canonical start title=%08lX%08lX start=%08lX%08lX\n",
      (unsigned long)(title_hash>>32),(unsigned long)title_hash,
      (unsigned long)(start_hash>>32),(unsigned long)start_hash);
  } else {
    Xbox_Log("ZNET R49.5 instant canonical title restore hash=%08lX%08lX\n",
      (unsigned long)(title_hash>>32),(unsigned long)title_hash);
  }
  return true;
}
static bool TitleRestoreCanonical(void) {
  if(TitleCacheRestore(false))return true;
  if(!zamn_runtime_reset_to_title(s_title_runtime,false))return false;
  TitleCacheWriteCanonical();return true;
}

static void TitleBackground(uint8_t* pixels,int pitch) {
  if(!s_title_prepared) {
    zamn_runtime_title_control(s_title_runtime,true,-1);
    zamn_runtime_set_pad(s_title_runtime,0,0);
    zamn_runtime_set_pad(s_title_runtime,1,0);
    ZamnFrameResult fr;
    // Same cached 256-wide path as the normal title. No GPU texture is locked.
    if(!zamn_runtime_frame(s_title_runtime,s_native_stage,256*4,&fr))return;
    if(!fr.native_video) {
      zamn_runtime_copy_legacy_frame(s_title_runtime,s_legacy_stage,ZAMN_FB_W*4,NULL,0,ZAMN_FB_H);
      for(int y=0;y<224;++y)for(int x=0;x<256;++x)
        ((uint32_t*)s_native_stage)[y*256+x]=((uint32_t*)s_legacy_stage)[(y*2+16)*512+x*2];
    }
    int16_t audio[ZAMN_AUDIO_SAMPLES_PER_FRAME*2];
    zamn_runtime_audio(s_title_runtime,audio,ZAMN_AUDIO_SAMPLES_PER_FRAME);
    Xbox_Audio_Submit(audio,ZAMN_AUDIO_SAMPLES_PER_FRAME);
  }
  for(int y=0;y<224;++y)memcpy(pixels+y*pitch,s_native_stage+y*256*4,256*4);
}
static void R4992_ShowLoadingOverlay(void) {
  // Present one immediate frame before a synchronous return-to-title operation
  // so players get positive feedback even if cache generation/network teardown
  // takes several seconds. The overlay darkens the last rendered game frame in
  // software, giving the same translucent look as the existing online menus.
  uint8_t* target=NULL;int pitch=0;Xbox_D3D_BeginDraw(&target,&pitch);
  if(target&&pitch>=256*4){
    for(int y=0;y<224;++y)memcpy(target+y*pitch,s_native_stage+y*256*4,256*4);
    for(int y=0;y<224;++y){uint32_t* row=(uint32_t*)(target+y*pitch);for(int x=0;x<256;++x)row[x]=(row[x]>>2)&0x003f3f3fu;}
    for(int y=82;y<142;++y){uint32_t* row=(uint32_t*)(target+y*pitch);for(int x=54;x<202;++x)row[x]=(row[x]>>1)&0x007f7f7fu;}
    Xbox_Netplay_DrawText(target,pitch,73,103,"LOADING...",2,0x00FFFFFFu);
  }
  Xbox_D3D_SetSourceRect(0,0,256,224);Xbox_D3D_PrepareFrame();Xbox_D3D_Present();
}

static int TitlePrepareMatch(bool begin) {
  static unsigned steps=0;
  if(begin) {
    s_title_prepared=true;steps=0;
    Xbox_Audio_SetMuted(true);
    Xbox_Log("ZNET R49.6 start audio muted until gameplay release\n");
    if(TitleCacheRestore(true)){Xbox_FlushLog();return 100;}
    zamn_runtime_title_reset_begin(s_title_runtime);
    Xbox_Log("ZNET R49.5 canonical cache unavailable; fallback prepare begun\n");Xbox_FlushLog();
    return 0;
  }
  const DWORD tick=GetTickCount();
  for(int i=0;i<8;++i) {
    // Stop at the canonical title *before* START. That snapshot is reusable for
    // online starts and for returning to the frontend after a match.
    const int result=zamn_runtime_title_reset_step(s_title_runtime,false);++steps;
    if(result!=0) {
      if(result<0){Xbox_Log("ZNET R49.5 canonical prepare FAILED frames=%u\n",steps);Xbox_FlushLog();return -1;}
      const uint64_t title_hash=zamn_runtime_state_hash(s_title_runtime);
      TitleCacheWriteCanonical();
      if(!zamn_runtime_title_start(s_title_runtime)){Xbox_Log("ZNET R49.5 canonical START pulse FAILED\n");Xbox_FlushLog();return -1;}
      const uint64_t start_hash=zamn_runtime_state_hash(s_title_runtime);
      Xbox_Log("ZNET R49.5 canonical prepare ready frames=%u title=%08lX%08lX start=%08lX%08lX\n",
        steps,(unsigned long)(title_hash>>32),(unsigned long)title_hash,
        (unsigned long)(start_hash>>32),(unsigned long)start_hash);
      Xbox_FlushLog();return 100;
    }
    if(GetTickCount()-tick>=6)break;
  }
  // R49.4 measured the USA neutral title entry at frame 2224. This percentage
  // is now only the one-time cache-seeding fallback path.
  unsigned percent=steps*100/2224;return percent<100?(int)percent:99;
}
static void R71_ShowGameManual(void) {
  const uint8_t initial=Xbox_Input_ManualButtons(0);
  uint8_t previous=initial;
  int spread=0;
  bool opened=Xbox_D3D_ManualOpen(spread);
  if(!opened) { Xbox_Log("R71MANUAL missing booklet spread assets\n"); return; }
  Xbox_Audio_SetMuted(true);
  Xbox_Log("R72MANUAL opened spreads=10 zoom-pan=ON\n");
  // Explicit button releases keep the same held LT/RT from turning pages repeatedly.
  for(;;) {
    Xbox_Input_Poll();
    const uint8_t now=Xbox_Input_ManualButtons(0);
    const uint8_t edge=(uint8_t)(now & ~previous);
    previous=now;
    if((edge & (4u|8u)) != 0 || Xbox_Input_ExitRequested()) break;
    const int old=spread;
    if((edge&1u) && spread>0)--spread;
    if((edge&2u) && spread<9)++spread;
    if(old!=spread) { if(!Xbox_D3D_ManualPage(spread)) spread=old; }
    // Manual input is local and title-only. Trigger actions use fresh edges.
    if(edge & 16u)Xbox_D3D_ManualZoom(1);
    if(edge & 32u)Xbox_D3D_ManualZoom(-1);
    if(edge & 64u)Xbox_D3D_ManualResetView();
    Xbox_D3D_ManualPan(Xbox_Input_ManualPanX(0),Xbox_Input_ManualPanY(0));
    Xbox_D3D_Present();
    Sleep(1);
  }
  Xbox_D3D_ManualClose();
  Xbox_Audio_SetMuted(false);
  Xbox_Log("R72MANUAL closed\n");
}

static void TitleOverlay(uint8_t* pixels,int pitch,bool native) {
  if(!pixels || !s_title_menu.visible)return;
  const int scale=native?1:2, top=native?0:16;
  const int selected=s_title_menu.online?s_title_menu.online_selection:s_title_menu.selection;

  // R49.9.4: only the MAIN and ONLINE menus use the compact two-row layout.
  // Keeping them in the last ~30 source pixels leaves the large title artwork
  // readable instead of stacking six choices over NEIGHBORS.
  if(!s_title_menu.online) {
    const char* labels[]={"START","ONLINE","PASSWORD","GAME MANUAL"};
    const int xs[]={83,146,55,146};
    const int ys[]={194,194,210,210};
    for(int i=0;i<4;++i){
      const int x=xs[i]*scale,y=ys[i]*scale+top;
      Xbox_Netplay_DrawText(pixels,pitch,x+scale,y+scale,labels[i],scale,0x00001800);
      Xbox_Netplay_DrawText(pixels,pitch,x,y,labels[i],scale,i==selected?0x00FFFFFF:0x0068FF68);
      if(i==selected)Xbox_Netplay_DrawText(pixels,pitch,x-8*scale,y,">",scale,0x00FFFFFF);
    }
    return;
  }

  // Row 0: PUBLIC ROOMS | DIRECT CONNECT
  // Row 1: SOLO PLAY | LEADERBOARDS | PROFILE
  const char* labels[]={"PUBLIC ROOMS","DIRECT CONNECT","SOLO PLAY","LEADERBOARDS","PROFILE"};
  const int xs[]={44,128,32,98,182};
  const int ys[]={194,194,210,210,210};
  for(int i=0;i<5;++i){
    const int x=xs[i]*scale,y=ys[i]*scale+top;
    Xbox_Netplay_DrawText(pixels,pitch,x+scale,y+scale,labels[i],scale,0x00001800);
    Xbox_Netplay_DrawText(pixels,pitch,x,y,labels[i],scale,i==selected?0x00FFFFFF:0x0068FF68);
    if(i==selected)Xbox_Netplay_DrawText(pixels,pitch,x-8*scale,y,">",scale,0x00FFFFFF);
  }
}


#ifdef ZAMN_R48_ROLLBACK
static void R48_RollbackFree(void);
static bool R48_RollbackInit(ZamnNativeRuntime* runtime, uint8_t window);
#endif

// ---------------------------------------------------------------------------
// R49.7 online shared save/load pause menu.
// START+BACK is encoded by xbox_input.cpp as this out-of-band pad bit.
// It is part of the deterministic netplay input stream but is never forwarded
// to the SNES runtime.
#define R498_PAD_SYS_EXIT 0x4000u
#define R497_PAD_SYS_PAUSE 0x8000u
#define R497_PAD_GAME_MASK 0x1fffu // R71: bit12 carries deterministic LT previous-weapon edge
#define R498_SAVE_MAGIC 0x3856535au /* 'ZSV8' */
#define R498_SAVE_SCHEMA 3u
#define R498_SAVE_PREV_SCHEMA 2u
#define R498_SAVE_BUILD 0x5A4D4908u
#define R497_NAME_MAX 16
#define R497_SAVE_PAGE 6

struct R498LevelClock {
  uint16_t level;
  uint16_t exit_last;
  uint32_t frames;
  uint8_t exit_mask;
  bool active;
  bool score_recorded;
};
static bool s_r498_solo_session=false;
static uint8_t s_r498_ruleset=ZAMN_RULE_NORMAL_SAVE;
static R498LevelClock s_r498_clock={0,0,0,0,false,false};

#define R498_PENDING_SCORE_MAX 8
struct R498PendingScore {
  uint32_t frame;
  uint32_t frames;
  uint16_t level;
  uint8_t ruleset;
  uint8_t players;
  bool solo;
  bool valid;
};
static R498PendingScore s_r498_pending_scores[R498_PENDING_SCORE_MAX];

static void R498_PendingScoresClear(void){memset(s_r498_pending_scores,0,sizeof(s_r498_pending_scores));}
static void R498_PendingScoresDiscardFrom(uint32_t frame){
  for(int i=0;i<R498_PENDING_SCORE_MAX;++i)
    if(s_r498_pending_scores[i].valid&&s_r498_pending_scores[i].frame>=frame)
      s_r498_pending_scores[i].valid=false;
}
static void R498_QueuePendingScore(uint8_t ruleset,uint16_t level,uint32_t frames,
                                   uint8_t players,bool solo,uint32_t frame){
  for(int i=0;i<R498_PENDING_SCORE_MAX;++i){
    R498PendingScore& q=s_r498_pending_scores[i];
    if(q.valid&&q.frame==frame&&q.level==level&&q.ruleset==(ruleset&3u)){
      q.frames=frames;q.players=players;q.solo=solo;return;
    }
  }
  for(int i=0;i<R498_PENDING_SCORE_MAX;++i){
    R498PendingScore& q=s_r498_pending_scores[i];
    if(!q.valid){q.frame=frame;q.frames=frames;q.level=level;q.ruleset=(uint8_t)(ruleset&3u);q.players=players;q.solo=solo;q.valid=true;return;}
  }
}
static void R498_CommitAuthoritativeScores(void){
  for(int i=0;i<R498_PENDING_SCORE_MAX;++i){
    R498PendingScore& q=s_r498_pending_scores[i];
    if(q.valid&&Xbox_Netplay_FrameAuthoritative(q.frame)){
      Xbox_Netplay_RecordLevelTime(q.ruleset,q.level,q.frames,q.players,q.solo);
      q.valid=false;
    }
  }
}

enum R497ControlKind {
  R497_CTRL_MENU_STATE = 1,
  R497_CTRL_SAVE_COMMAND = 2,
  R497_CTRL_LOAD_COMMAND = 3,
  R497_CTRL_BARRIER = 4,
  R497_CTRL_RESUME = 5,
};

enum R497MenuScreen {
  R497_MENU_MAIN = 0,
  R497_MENU_KEYBOARD = 1,
  R497_MENU_LOAD = 2,
};

struct R497SaveHeader {
  uint32_t magic;
  uint32_t schema;
  uint32_t build;
  uint32_t snapshot_size;
  uint32_t frame;
  uint8_t ruleset;
  uint8_t reserved_mode[3];
  uint16_t level;
  uint16_t reserved_level;
  uint32_t level_frames;
  uint64_t rom_hash;
  uint64_t state_hash;
  uint64_t blob_hash;
  char name[R497_NAME_MAX + 1];
  char reserved2[7];
};

struct R497StagedSave {
  R497SaveHeader h;
  uint8_t* data;
  int size;
};

struct R497MenuWire {
  uint32_t seq;
  uint8_t owner;
  uint8_t screen;
  uint8_t selection;
  uint8_t key_index;
  uint8_t list_count;
  uint8_t list_selected;
  uint8_t reserved[2];
  char name[R497_NAME_MAX + 1];
  char status[32];
  char list[R497_SAVE_PAGE][R497_NAME_MAX + 1];
};

struct R497CommandWire {
  uint32_t seq;
  uint32_t frame;
  char name[R497_NAME_MAX + 1];
  char reserved[3];
};

struct R497BarrierWire {
  uint32_t seq;
  uint32_t frame;
  uint64_t state_hash;
  uint8_t phase;
  uint8_t ok;
  uint8_t reserved[6];
};

static uint64_t R497_Fnv64(const uint8_t* data, int size) {
  uint64_t h = 14695981039346656037ull;
  for(int i=0;i<size;++i){h^=data[i];h*=1099511628211ull;}
  return h;
}

static bool R497_EnsureSaveDir(char* out_root, int cap) {
  static char s_root[MAX_PATH]={0};
  if(!out_root||cap<4)return false;
  if(!s_root[0]) {
    // Store public saves in the Xbox Dashboard-managed UDATA area. One
    // container owns all named online snapshots so users can copy/delete it
    // from the normal Xbox memory manager.
    const wchar_t* display=L"ZAMN - Online Saves";
    DWORD ret=XCreateSaveGame("U:\\",display,OPEN_ALWAYS,0,s_root,(UINT)sizeof(s_root));
    if(ret==ERROR_SUCCESS) {
      size_t n=strlen(s_root);
      if(n && s_root[n-1]!='\\' && n+1<sizeof(s_root)){s_root[n]='\\';s_root[n+1]=0;}
    } else {
      // Loose-HDD fallback only. Disc launches should normally use UDATA.
      strncpy(s_root,"D:\\ZAMN_SAVES\\",sizeof(s_root)-1);s_root[sizeof(s_root)-1]=0;
      CreateDirectoryA("D:\\ZAMN_SAVES",NULL);
      DWORD a=GetFileAttributesA("D:\\ZAMN_SAVES");
      if(a==0xffffffffu || !(a&FILE_ATTRIBUTE_DIRECTORY)){s_root[0]=0;return false;}
    }
  }
  strncpy(out_root,s_root,(size_t)cap-1);out_root[cap-1]=0;return true;
}

static void R497_SanitizeName(const char* in,char* out,int cap) {
  int n=0;
  if(out && cap>0)out[0]=0;
  for(const char* p=in;out && p && *p && n<cap-1 && n<R497_NAME_MAX;++p) {
    char c=*p;
    if(c>='a'&&c<='z')c=(char)(c-'a'+'A');
    if(c==' ')c='_';
    if((c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.')
      out[n++]=c;
  }
  if(out&&cap>0)out[n]=0;
}

static bool R497_SavePath(const char* name,char* out,int cap) {
  char root[MAX_PATH],clean[R497_NAME_MAX+1];
  if(!R497_EnsureSaveDir(root,sizeof(root)))return false;
  R497_SanitizeName(name,clean,sizeof(clean));
  if(!clean[0])return false;
  snprintf(out,(size_t)cap,"%s%s.zsv",root,clean);
  if(cap>0)out[cap-1]=0;
  return true;
}

static void R497_DeleteSave(const char* name) {
  char path[MAX_PATH];if(R497_SavePath(name,path,sizeof(path)))DeleteFileA(path);
}

static void R497_FreeStaged(R497StagedSave* st) {
  if(!st)return;
  if(st->data)free(st->data);
  memset(st,0,sizeof(*st));
}

static bool R497_WriteSave(ZamnNativeRuntime* runtime,uint32_t frame,const char* name,
                           uint8_t ruleset,const R498LevelClock* clock,uint64_t* out_hash) {
  if(!runtime||!name)return false;
  char path[MAX_PATH],clean[R497_NAME_MAX+1];
  if(!R497_SavePath(name,path,sizeof(path)))return false;
  R497_SanitizeName(name,clean,sizeof(clean));
  const int cap=zamn_runtime_snapshot_size(runtime);
  if(cap<=0)return false;
  uint8_t* data=(uint8_t*)malloc((size_t)cap);if(!data)return false;
  int used=0;
  if(!zamn_runtime_save_snapshot(runtime,data,cap,&used)||used<=0){free(data);return false;}
  R497SaveHeader h;memset(&h,0,sizeof(h));
  h.magic=R498_SAVE_MAGIC;h.schema=R498_SAVE_SCHEMA;h.build=R498_SAVE_BUILD;
  h.snapshot_size=(uint32_t)used;h.frame=frame;h.ruleset=(uint8_t)(ruleset&3u);
  if(clock&&clock->active){h.level=clock->level;h.level_frames=clock->frames;}
  h.rom_hash=s_title_rom_hash;
  h.state_hash=zamn_runtime_state_hash(runtime);h.blob_hash=R497_Fnv64(data,used);
  strncpy(h.name,clean,R497_NAME_MAX);h.name[R497_NAME_MAX]=0;
  FILE* f=fopen(path,"wb");
  bool ok=f && fwrite(&h,1,sizeof(h),f)==sizeof(h) &&
               fwrite(data,1,(size_t)used,f)==(size_t)used;
  if(f)fclose(f);free(data);
  if(out_hash)*out_hash=h.state_hash;
  return ok;
}

static bool R497_ReadSave(ZamnNativeRuntime* runtime,const char* name,uint8_t ruleset,R497StagedSave* out) {
  if(!runtime||!name||!out)return false;
  R497_FreeStaged(out);
  char path[MAX_PATH];if(!R497_SavePath(name,path,sizeof(path)))return false;
  FILE* f=fopen(path,"rb");if(!f)return false;
  R497SaveHeader h;memset(&h,0,sizeof(h));
  if(fread(&h,1,sizeof(h),f)!=sizeof(h)){fclose(f);return false;}
  const int cap=zamn_runtime_snapshot_size(runtime);
  if(h.magic!=R498_SAVE_MAGIC||(h.schema!=R498_SAVE_SCHEMA&&h.schema!=R498_SAVE_PREV_SCHEMA)||h.build!=R498_SAVE_BUILD||
     h.rom_hash!=s_title_rom_hash||(h.ruleset&3u)!=(ruleset&3u)||h.snapshot_size==0||cap<=0||
     h.snapshot_size>(uint32_t)cap){fclose(f);return false;}
  uint8_t* data=(uint8_t*)malloc(h.snapshot_size);if(!data){fclose(f);return false;}
  const bool ok=fread(data,1,h.snapshot_size,f)==h.snapshot_size;fclose(f);
  if(!ok||R497_Fnv64(data,(int)h.snapshot_size)!=h.blob_hash){free(data);return false;}
  out->h=h;out->data=data;out->size=(int)h.snapshot_size;return true;
}

static int R497_ListSaves(char out[][R497_NAME_MAX+1],int cap,uint8_t ruleset) {
  if(!out||cap<=0)return 0;
  char root[MAX_PATH],pattern[MAX_PATH+16];if(!R497_EnsureSaveDir(root,sizeof(root)))return 0;
  snprintf(pattern,sizeof(pattern),"%s*.zsv",root);
  WIN32_FIND_DATAA fd;HANDLE h=FindFirstFileA(pattern,&fd);if(h==INVALID_HANDLE_VALUE)return 0;
  int count=0;
  do {
    if(fd.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)continue;
    const char* fn=fd.cFileName;size_t n=strlen(fn);
    if(n<5||strcmp(fn+n-4,".zsv"))continue;
    char path[MAX_PATH];snprintf(path,sizeof(path),"%s%s",root,fn);path[sizeof(path)-1]=0;
    FILE* f=fopen(path,"rb");if(!f)continue;R497SaveHeader sh;memset(&sh,0,sizeof(sh));bool compatible=fread(&sh,1,sizeof(sh),f)==sizeof(sh);fclose(f);
    if(!compatible||sh.magic!=R498_SAVE_MAGIC||(sh.schema!=R498_SAVE_SCHEMA&&sh.schema!=R498_SAVE_PREV_SCHEMA)||sh.build!=R498_SAVE_BUILD||sh.rom_hash!=s_title_rom_hash||(sh.ruleset&3u)!=(ruleset&3u))continue;
    char tmp[R497_NAME_MAX+1];memset(tmp,0,sizeof(tmp));
    size_t take=n-4;if(take>R497_NAME_MAX)take=R497_NAME_MAX;
    memcpy(tmp,fn,take);tmp[take]=0;
    strncpy(out[count],tmp,R497_NAME_MAX);out[count][R497_NAME_MAX]=0;
    ++count;
  } while(count<cap && FindNextFileA(h,&fd));
  FindClose(h);
  return count;
}

static void R497_Rect(uint8_t* pixels,int pitch,int x,int y,int w,int h,uint32_t c) {
  if(!pixels||pitch<=0||w<=0||h<=0)return;
  for(int yy=0;yy<h;++yy){uint32_t* r=(uint32_t*)(pixels+(y+yy)*pitch);for(int xx=0;xx<w;++xx)r[x+xx]=c;}
}

static void R497_Darken(uint8_t* pixels,int pitch,int w,int h,int shift) {
  if(!pixels||pitch<=0)return;
  for(int y=0;y<h;++y){uint32_t* r=(uint32_t*)(pixels+y*pitch);for(int x=0;x<w;++x)r[x]=(r[x]>>shift)&(shift==1?0x007f7f7fu:0x003f3f3fu);}
}

static void R497_DarkenBox(uint8_t* pixels,int pitch,bool native,int x,int y,int w,int h) {
  const int scale=native?1:2,top=native?0:16;
  const int px=x*scale,py=y*scale+top,pw=w*scale,ph=h*scale;
  for(int yy=0;yy<ph;++yy){uint32_t* r=(uint32_t*)(pixels+(py+yy)*pitch);for(int xx=0;xx<pw;++xx)r[px+xx]=(r[px+xx]>>2)&0x003f3f3fu;}
}

static void R497_Text(uint8_t* pixels,int pitch,bool native,int x,int y,const char* t,uint32_t c) {
  const int scale=native?1:2,top=native?0:16;
  Xbox_Netplay_DrawText(pixels,pitch,x*scale,y*scale+top,t,scale,c);
}

static void R497_Box(uint8_t* pixels,int pitch,bool native,int x,int y,int w,int h,uint32_t c) {
  const int scale=native?1:2,top=native?0:16;
  R497_Rect(pixels,pitch,x*scale,y*scale+top,w*scale,h*scale,c);
}

static const char* R497_Keys(void) {
  return "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_. ";
}

static void R497_DrawPauseMenu(ZamnNativeRuntime* runtime,bool last_native,
                               const R497MenuWire* m,bool local_owner,bool savable) {
  uint8_t* pixels=0;int pitch=0;Xbox_D3D_BeginDraw(&pixels,&pitch);
  if(!pixels)return;
  if(last_native) {
    for(int y=0;y<224;++y)memcpy(pixels+y*pitch,s_native_stage+y*256*4,256*4);
    Xbox_D3D_SetSourceRect(0,0,256,224);
    R497_Darken(pixels,pitch,256,224,1);
  } else {
    zamn_runtime_copy_legacy_frame(runtime,pixels,pitch,s_legacy_stage,ZAMN_FB_W*4,ZAMN_FB_H);
    Xbox_D3D_SetSourceRect(0,16,512,448);
    R497_Darken(pixels,pitch,512,480,1);
  }
  const bool native=last_native;
  R497_DarkenBox(pixels,pitch,native,16,16,224,194);
  R497_Box(pixels,pitch,native,17,17,222,1,0x0068FF68u);
  R497_Box(pixels,pitch,native,17,208,222,1,0x0068FF68u);
  R497_Text(pixels,pitch,native,26,27,"ONLINE PAUSE",0x00FFFFFFu);
  R497_Text(pixels,pitch,native,150,27,local_owner?"LOCAL CONTROL":"REMOTE CONTROL",0x0068FF68u);

  if(m->screen==R497_MENU_MAIN) {
    if(!savable) {
      R497_Text(pixels,pitch,native,42,70,"NO-SAVE RULESET",0x00FFD35Au);
      R497_Box(pixels,pitch,native,34,112,188,16,0x00304030u);
      R497_Text(pixels,pitch,native,48,116,"RESUME GAME",0x00FFFFFFu);
      R497_Text(pixels,pitch,native,35,116,">",0x00FFFFFFu);
      R497_Text(pixels,pitch,native,28,192,"A/B RESUME",0x00C8D4DCu);
    } else {
      const char* rows[]={"SAVE GAME","LOAD GAME","RESUME GAME"};
      for(int i=0;i<3;++i) {
        const int y=72+i*28;
        if(i==m->selection)R497_Box(pixels,pitch,native,34,y-4,188,16,0x00304030u);
        R497_Text(pixels,pitch,native,48,y,rows[i],i==m->selection?0x00FFFFFFu:0x0068FF68u);
        if(i==m->selection)R497_Text(pixels,pitch,native,35,y,">",0x00FFFFFFu);
      }
      if(m->status[0])R497_Text(pixels,pitch,native,28,168,m->status,0x00FFD35Au);
      R497_Text(pixels,pitch,native,28,192,"A SELECT   B RESUME",0x00C8D4DCu);
    }
  } else if(m->screen==R497_MENU_KEYBOARD) {
    char shown[32];snprintf(shown,sizeof(shown),"NAME: %s_",m->name);
    R497_Text(pixels,pitch,native,28,48,shown,0x00FFD35Au);
    const char* keys=R497_Keys();
    for(int i=0;i<40;++i) {
      const int row=i/8,col=i%8,x=42+col*22,y=76+row*20;
      char ch[2]={keys[i],0};if(ch[0]==' ')ch[0]='_';
      if(i==m->key_index)R497_Box(pixels,pitch,native,x-4,y-4,14,14,0x00404020u);
      R497_Text(pixels,pitch,native,x,y,ch,i==m->key_index?0x00FFFFFFu:0x0068FF68u);
    }
    R497_Text(pixels,pitch,native,26,184,"A TYPE  X DELETE  START DONE",0x00C8D4DCu);
    R497_Text(pixels,pitch,native,26,197,"B CANCEL",0x00C8D4DCu);
  } else if(m->screen==R497_MENU_LOAD) {
    R497_Text(pixels,pitch,native,28,49,"SELECT SAVE",0x00FFD35Au);
    if(!m->list_count)R497_Text(pixels,pitch,native,42,88,"NO SAVES FOUND",0x0068FF68u);
    for(int i=0;i<m->list_count && i<R497_SAVE_PAGE;++i) {
      const int y=76+i*18;
      if(i==m->list_selected)R497_Box(pixels,pitch,native,32,y-3,190,14,0x00304030u);
      R497_Text(pixels,pitch,native,44,y,m->list[i],
                i==m->list_selected?0x00FFFFFFu:0x0068FF68u);
      if(i==m->list_selected)R497_Text(pixels,pitch,native,34,y,">",0x00FFFFFFu);
    }
    R497_Text(pixels,pitch,native,28,192,"A LOAD   B BACK",0x00C8D4DCu);
  }
  Xbox_D3D_PrepareFrame();Xbox_D3D_Present();
}

static bool R497_Barrier(uint32_t seq,uint8_t phase,bool ok,uint32_t frame,uint64_t hash,
                         bool* peer_ok,uint32_t* peer_frame,uint64_t* peer_hash) {
  R497BarrierWire mine;memset(&mine,0,sizeof(mine));
  mine.seq=seq;mine.phase=phase;mine.ok=ok?1u:0u;mine.frame=frame;mine.state_hash=hash;
  DWORD start=GetTickCount(),last=0;
  while(GetTickCount()-start<8000u) {
    DWORD now=GetTickCount();
    if(!last||now-last>=24u){Xbox_Netplay_ControlSend(R497_CTRL_BARRIER,&mine,sizeof(mine));last=now;}
    uint8_t kind=0;R497BarrierWire peer;memset(&peer,0,sizeof(peer));
    int n=Xbox_Netplay_ControlRecv(&kind,&peer,sizeof(peer),12);
    if(n<0)return false;
    if(kind==R497_CTRL_BARRIER && n==(int)sizeof(peer) && peer.seq==seq && peer.phase==phase) {
      if(peer_ok)*peer_ok=peer.ok!=0;if(peer_frame)*peer_frame=peer.frame;if(peer_hash)*peer_hash=peer.state_hash;
      // One extra copy lets the peer escape its barrier even if its first copy
      // of our status was the packet that unblocked us.
      Xbox_Netplay_ControlSend(R497_CTRL_BARRIER,&mine,sizeof(mine));
      return true;
    }
  }
  return false;
}

static void R497_SendCommand(uint8_t kind,const R497CommandWire* cmd) {
  for(int i=0;i<8;++i){Xbox_Netplay_ControlSend(kind,cmd,sizeof(*cmd));Sleep(5);}
}

static bool R497_SaveBackup(ZamnNativeRuntime* runtime,R497StagedSave* out,uint32_t frame) {
  R497_FreeStaged(out);const int cap=zamn_runtime_snapshot_size(runtime);if(cap<=0)return false;
  uint8_t* data=(uint8_t*)malloc((size_t)cap);if(!data)return false;int used=0;
  if(!zamn_runtime_save_snapshot(runtime,data,cap,&used)||used<=0){free(data);return false;}
  memset(&out->h,0,sizeof(out->h));out->h.frame=frame;out->h.state_hash=zamn_runtime_state_hash(runtime);
  out->data=data;out->size=used;return true;
}

static bool R497_ApplyStaged(ZamnNativeRuntime* runtime,const R497StagedSave* st) {
  return runtime&&st&&st->data&&st->size>0 &&
         zamn_runtime_load_snapshot(runtime,st->data,st->size) &&
         (st->h.schema==R498_SAVE_PREV_SCHEMA
           ? zamn_runtime_legacy_state_hash(runtime)==st->h.state_hash
           : zamn_runtime_state_hash(runtime)==st->h.state_hash);
}

// Returns true when gameplay should resume. `io_frame` is changed after a
// successful load. Both consoles execute this routine on the same logical
// frame, so the game runtime remains frozen for the full menu lifetime.
static bool R497_RunOnlineSaveMenu(ZamnNativeRuntime* runtime,uint32_t* io_frame,
                                   uint8_t owner_slot,bool last_native,uint8_t ruleset,
                                   R498LevelClock* clock,bool* out_loaded) {
  if(out_loaded)*out_loaded=false;
  if(!runtime||!io_frame)return false;
  const uint8_t local_slot=Xbox_Netplay_LocalSlot();
  const bool local_owner=local_slot==owner_slot;
  const bool savable=Xbox_Netplay_RulesetSavable(ruleset);
  R497MenuWire m;memset(&m,0,sizeof(m));m.owner=owner_slot;m.screen=R497_MENU_MAIN;
  uint16_t prev=0;uint32_t seq=1;bool running=true,loaded=false,fatal=false;
  char saves[32][R497_NAME_MAX+1];int save_count=0,load_index=0;
  R497StagedSave staged;memset(&staged,0,sizeof(staged));
  R497StagedSave backup;memset(&backup,0,sizeof(backup));
  Xbox_Audio_SetMuted(true);

  // Refuse to open a shared save menu unless both peers prove they are paused
  // on the same runtime state. This protects saves from an unresolved rollback
  // correction or any pre-existing desync.
  {
    const uint32_t gate_seq=0x80000000u^(*io_frame);
    const uint64_t gate_hash=zamn_runtime_state_hash(runtime);
    bool pok=false;uint32_t pf=0;uint64_t ph=0;
    if(!R497_Barrier(gate_seq,0,true,*io_frame,gate_hash,&pok,&pf,&ph) ||
       !pok||pf!=*io_frame||ph!=gate_hash) {
      Xbox_Audio_SetMuted(false);
      return false;
    }
  }

  while(running) {
    if(local_owner) {
      Xbox_Input_Poll();uint16_t pad=(uint16_t)(Xbox_Input_GetJoypad(0)&R497_PAD_GAME_MASK);
      uint16_t press=(uint16_t)(pad&~prev);prev=pad;
      if(m.screen==R497_MENU_MAIN) {
        if(savable && (press&(1u<<4))){if(m.selection>0)--m.selection;m.status[0]=0;}
        if(savable && (press&(1u<<5))){if(m.selection<2)++m.selection;m.status[0]=0;}
        if(press&(1u<<8)){ // Xbox B
          R497CommandWire cmd;memset(&cmd,0,sizeof(cmd));cmd.seq=++seq;cmd.frame=*io_frame;
          R497_SendCommand(R497_CTRL_RESUME,&cmd);
          const uint64_t resume_hash=zamn_runtime_state_hash(runtime);
          bool pok=false;uint32_t pf=0;uint64_t ph=0;
          const bool synced=R497_Barrier(cmd.seq,9,true,*io_frame,resume_hash,&pok,&pf,&ph);
          if(!synced||!pok||pf!=*io_frame||ph!=resume_hash)fatal=true;
          running=false;
        } else if(press&(1u<<0)) { // Xbox A
          if(!savable) {
            R497CommandWire cmd;memset(&cmd,0,sizeof(cmd));cmd.seq=++seq;cmd.frame=*io_frame;
            R497_SendCommand(R497_CTRL_RESUME,&cmd);
            const uint64_t resume_hash=zamn_runtime_state_hash(runtime);
            bool pok=false;uint32_t pf=0;uint64_t ph=0;
            const bool synced=R497_Barrier(cmd.seq,9,true,*io_frame,resume_hash,&pok,&pf,&ph);
            if(!synced||!pok||pf!=*io_frame||ph!=resume_hash)fatal=true;
            running=false;
          } else if(m.selection==0){m.screen=R497_MENU_KEYBOARD;m.key_index=0;m.name[0]=0;prev=pad;}
          else if(m.selection==1){
            save_count=R497_ListSaves(saves,32,ruleset);load_index=0;m.screen=R497_MENU_LOAD;prev=pad;
          } else {
            R497CommandWire cmd;memset(&cmd,0,sizeof(cmd));cmd.seq=++seq;cmd.frame=*io_frame;
            R497_SendCommand(R497_CTRL_RESUME,&cmd);
            const uint64_t resume_hash=zamn_runtime_state_hash(runtime);
            bool pok=false;uint32_t pf=0;uint64_t ph=0;
            const bool synced=R497_Barrier(cmd.seq,9,true,*io_frame,resume_hash,&pok,&pf,&ph);
            if(!synced||!pok||pf!=*io_frame||ph!=resume_hash)fatal=true;
            running=false;
          }
        }
      } else if(m.screen==R497_MENU_KEYBOARD) {
        int k=m.key_index,row=k/8,col=k%8;
        if(press&(1u<<4)){row=(row+4)%5;m.key_index=(uint8_t)(row*8+col);}
        if(press&(1u<<5)){row=(row+1)%5;m.key_index=(uint8_t)(row*8+col);}
        if(press&(1u<<6)){col=(col+7)%8;m.key_index=(uint8_t)(row*8+col);}
        if(press&(1u<<7)){col=(col+1)%8;m.key_index=(uint8_t)(row*8+col);}
        if(press&(1u<<1)){size_t n=strlen(m.name);if(n)m.name[n-1]=0;} // Xbox X
        if(press&(1u<<8)){m.screen=R497_MENU_MAIN;m.name[0]=0;} // Xbox B
        if(press&(1u<<0)){
          size_t n=strlen(m.name);if(n<R497_NAME_MAX){
            char c=R497_Keys()[m.key_index];if(c==' '&&n==0){}else{m.name[n]=c;m.name[n+1]=0;}
          }
        }
        if((press&(1u<<3)) && m.name[0]) { // START = done
          char clean[R497_NAME_MAX+1];R497_SanitizeName(m.name,clean,sizeof(clean));
          if(clean[0]) {
            R497CommandWire cmd;memset(&cmd,0,sizeof(cmd));cmd.seq=++seq;cmd.frame=*io_frame;
            strncpy(cmd.name,clean,R497_NAME_MAX);R497_SendCommand(R497_CTRL_SAVE_COMMAND,&cmd);
            uint64_t local_hash=0;bool lok=R497_WriteSave(runtime,*io_frame,clean,ruleset,clock,&local_hash);
            bool pok=false;uint32_t pf=0;uint64_t ph=0;
            bool synced=R497_Barrier(cmd.seq,1,lok,*io_frame,local_hash,&pok,&pf,&ph);
            memset(m.status,0,sizeof(m.status));
            if(synced&&lok&&pok&&pf==*io_frame&&ph==local_hash)
              strncpy(m.status,"SAVE COMPLETE",sizeof(m.status)-1);
            else {
              if(lok)R497_DeleteSave(clean);
              strncpy(m.status,"SAVE FAILED / PEER MISMATCH",sizeof(m.status)-1);
            }
            m.screen=R497_MENU_MAIN;m.selection=0;m.name[0]=0;
          }
        }
      } else if(m.screen==R497_MENU_LOAD) {
        if(press&(1u<<8)){m.screen=R497_MENU_MAIN;}
        if(save_count) {
          if(press&(1u<<4)){load_index=(load_index+save_count-1)%save_count;}
          if(press&(1u<<5)){load_index=(load_index+1)%save_count;}
          if(press&(1u<<0)) {
            const char* chosen=saves[load_index];
            R497CommandWire cmd;memset(&cmd,0,sizeof(cmd));cmd.seq=++seq;cmd.frame=*io_frame;
            strncpy(cmd.name,chosen,R497_NAME_MAX);R497_SendCommand(R497_CTRL_LOAD_COMMAND,&cmd);
            R497_FreeStaged(&staged);bool lok=R497_ReadSave(runtime,chosen,ruleset,&staged);
            bool pok=false;uint32_t pf=0;uint64_t ph=0;
            bool ready=R497_Barrier(cmd.seq,2,lok,lok?staged.h.frame:0,lok?staged.h.state_hash:0,&pok,&pf,&ph);
            bool same=ready&&lok&&pok&&pf==staged.h.frame&&ph==staged.h.state_hash;
            bool attempted=false,applied=false;
            if(same) {
              attempted=R497_SaveBackup(runtime,&backup,*io_frame);
              applied=attempted&&R497_ApplyStaged(runtime,&staged);
            }
            bool peer_applied=false;uint32_t paf=0;uint64_t pah=0;
            bool committed=R497_Barrier(cmd.seq,3,applied,
                applied?staged.h.frame:*io_frame,
                applied?zamn_runtime_state_hash(runtime):0,
                &peer_applied,&paf,&pah);
            if(committed&&applied&&peer_applied&&paf==staged.h.frame&&pah==staged.h.state_hash) {
              *io_frame=staged.h.frame;
              if(clock){clock->level=staged.h.level;clock->frames=staged.h.level_frames;clock->active=staged.h.level>=1&&staged.h.level<=56;clock->exit_last=zamn_runtime_exit_door_last(runtime);clock->exit_mask=0;clock->score_recorded=false;}
              loaded=true;running=false;
            } else {
              if(attempted&&backup.data&&!R497_ApplyStaged(runtime,&backup))fatal=true;
              memset(m.status,0,sizeof(m.status));
              strncpy(m.status,"LOAD FAILED / SAVE NOT ON BOTH",sizeof(m.status)-1);
              m.screen=R497_MENU_MAIN;
            }
            R497_FreeStaged(&staged);R497_FreeStaged(&backup);
          }
        }
      }

      // Publish enough UI state for the non-owner to mirror the same overlay.
      m.seq=seq;m.list_count=0;m.list_selected=0;
      memset(m.list,0,sizeof(m.list));
      if(m.screen==R497_MENU_LOAD&&save_count) {
        int first=load_index-(R497_SAVE_PAGE/2);if(first<0)first=0;
        if(first>save_count-R497_SAVE_PAGE)first=save_count-R497_SAVE_PAGE;
        if(first<0)first=0;
        int show=save_count-first;if(show>R497_SAVE_PAGE)show=R497_SAVE_PAGE;
        m.list_count=(uint8_t)show;m.list_selected=(uint8_t)(load_index-first);
        for(int i=0;i<show;++i)strncpy(m.list[i],saves[first+i],R497_NAME_MAX);
      }
      Xbox_Netplay_ControlSend(R497_CTRL_MENU_STATE,&m,sizeof(m));
    } else {
      uint8_t kind=0;uint8_t buf[sizeof(R497MenuWire)];memset(buf,0,sizeof(buf));
      int n=Xbox_Netplay_ControlRecv(&kind,buf,sizeof(buf),18);
      if(n<0){running=false;break;}
      if(kind==R497_CTRL_MENU_STATE&&n==(int)sizeof(R497MenuWire)) {
        memcpy(&m,buf,sizeof(m));
      } else if(kind==R497_CTRL_SAVE_COMMAND&&n==(int)sizeof(R497CommandWire)) {
        R497CommandWire cmd;memcpy(&cmd,buf,sizeof(cmd));
        uint64_t local_hash=0;bool lok=R497_WriteSave(runtime,*io_frame,cmd.name,ruleset,clock,&local_hash);
        bool pok=false;uint32_t pf=0;uint64_t ph=0;
        bool synced=R497_Barrier(cmd.seq,1,lok,*io_frame,local_hash,&pok,&pf,&ph);
        if(!(synced&&lok&&pok&&pf==*io_frame&&ph==local_hash) && lok)R497_DeleteSave(cmd.name);
      } else if(kind==R497_CTRL_LOAD_COMMAND&&n==(int)sizeof(R497CommandWire)) {
        R497CommandWire cmd;memcpy(&cmd,buf,sizeof(cmd));
        R497_FreeStaged(&staged);bool lok=R497_ReadSave(runtime,cmd.name,ruleset,&staged);
        bool pok=false;uint32_t pf=0;uint64_t ph=0;
        bool ready=R497_Barrier(cmd.seq,2,lok,lok?staged.h.frame:0,lok?staged.h.state_hash:0,&pok,&pf,&ph);
        bool same=ready&&lok&&pok&&pf==staged.h.frame&&ph==staged.h.state_hash;
        bool attempted=false,applied=false;
        if(same){attempted=R497_SaveBackup(runtime,&backup,*io_frame);applied=attempted&&R497_ApplyStaged(runtime,&staged);}
        bool peer_applied=false;uint32_t paf=0;uint64_t pah=0;
        bool committed=R497_Barrier(cmd.seq,3,applied,
            applied?staged.h.frame:*io_frame,
            applied?zamn_runtime_state_hash(runtime):0,
            &peer_applied,&paf,&pah);
        if(committed&&applied&&peer_applied&&paf==staged.h.frame&&pah==staged.h.state_hash) {
          *io_frame=staged.h.frame;
          if(clock){clock->level=staged.h.level;clock->frames=staged.h.level_frames;clock->active=staged.h.level>=1&&staged.h.level<=56;clock->exit_last=zamn_runtime_exit_door_last(runtime);clock->exit_mask=0;clock->score_recorded=false;}
          loaded=true;running=false;
        } else if(attempted&&backup.data&&!R497_ApplyStaged(runtime,&backup))fatal=true;
        R497_FreeStaged(&staged);R497_FreeStaged(&backup);
      } else if(kind==R497_CTRL_RESUME&&n==(int)sizeof(R497CommandWire)) {
        R497CommandWire cmd;memcpy(&cmd,buf,sizeof(cmd));
        const uint64_t resume_hash=zamn_runtime_state_hash(runtime);
        bool pok=false;uint32_t pf=0;uint64_t ph=0;
        const bool synced=R497_Barrier(cmd.seq,9,true,*io_frame,resume_hash,&pok,&pf,&ph);
        if(!synced||!pok||pf!=*io_frame||ph!=resume_hash)fatal=true;
        running=false;
      }
    }
    if(running)R497_DrawPauseMenu(runtime,last_native,&m,local_owner,savable);
  }

  R497_FreeStaged(&staged);R497_FreeStaged(&backup);
  Xbox_Audio_SetMuted(false);
  if(fatal)return false;
  if(loaded) {
    if(!Xbox_Netplay_ResyncAfterLoad(*io_frame))return false;
#ifdef ZAMN_R48_ROLLBACK
    R48_RollbackFree();
    if(Xbox_Netplay_RollbackEnabled()&&!R48_RollbackInit(runtime,Xbox_Netplay_RollbackWindow()))
      return false;
#endif
    if(out_loaded)*out_loaded=true;
  }
  return true;
}

// R49.8 Online Solo uses the same snapshot format/UI as shared online play,
// but needs no network barrier because there is only one deterministic owner.
static bool R498_RunSoloSaveMenu(ZamnNativeRuntime* runtime,uint32_t* io_frame,
                                 bool last_native,uint8_t ruleset,
                                 R498LevelClock* clock,bool* out_loaded) {
  if(out_loaded)*out_loaded=false;
  if(!runtime||!io_frame)return false;
  const bool savable=Xbox_Netplay_RulesetSavable(ruleset);
  R497MenuWire m;memset(&m,0,sizeof(m));m.owner=0;m.screen=R497_MENU_MAIN;
  uint16_t prev=0;bool running=true,loaded=false,fatal=false;
  char saves[32][R497_NAME_MAX+1];int save_count=0,load_index=0;
  R497StagedSave staged;memset(&staged,0,sizeof(staged));
  R497StagedSave backup;memset(&backup,0,sizeof(backup));
  Xbox_Audio_SetMuted(true);
  while(running) {
    Xbox_Input_Poll();uint16_t pad=(uint16_t)(Xbox_Input_GetJoypad(0)&R497_PAD_GAME_MASK);
    uint16_t press=(uint16_t)(pad&~prev);prev=pad;
    if(m.screen==R497_MENU_MAIN) {
      if(savable&&(press&(1u<<4))){if(m.selection>0)--m.selection;m.status[0]=0;}
      if(savable&&(press&(1u<<5))){if(m.selection<2)++m.selection;m.status[0]=0;}
      if(press&(1u<<8))running=false;
      else if(press&(1u<<0)) {
        if(!savable)running=false;
        else if(m.selection==0){m.screen=R497_MENU_KEYBOARD;m.key_index=0;m.name[0]=0;prev=pad;}
        else if(m.selection==1){save_count=R497_ListSaves(saves,32,ruleset);load_index=0;m.screen=R497_MENU_LOAD;prev=pad;}
        else running=false;
      }
    } else if(m.screen==R497_MENU_KEYBOARD) {
      int k=m.key_index,row=k/8,col=k%8;
      if(press&(1u<<4)){row=(row+4)%5;m.key_index=(uint8_t)(row*8+col);}
      if(press&(1u<<5)){row=(row+1)%5;m.key_index=(uint8_t)(row*8+col);}
      if(press&(1u<<6)){col=(col+7)%8;m.key_index=(uint8_t)(row*8+col);}
      if(press&(1u<<7)){col=(col+1)%8;m.key_index=(uint8_t)(row*8+col);}
      if(press&(1u<<1)){size_t n=strlen(m.name);if(n)m.name[n-1]=0;}
      if(press&(1u<<8)){m.screen=R497_MENU_MAIN;m.name[0]=0;}
      if(press&(1u<<0)){size_t n=strlen(m.name);if(n<R497_NAME_MAX){char c=R497_Keys()[m.key_index];if(c!=' '||n){m.name[n]=c;m.name[n+1]=0;}}}
      if((press&(1u<<3))&&m.name[0]) {
        char clean[R497_NAME_MAX+1];R497_SanitizeName(m.name,clean,sizeof(clean));
        uint64_t h=0;bool ok=clean[0]&&R497_WriteSave(runtime,*io_frame,clean,ruleset,clock,&h);
        memset(m.status,0,sizeof(m.status));strncpy(m.status,ok?"SAVE COMPLETE":"SAVE FAILED",sizeof(m.status)-1);
        m.screen=R497_MENU_MAIN;m.selection=0;m.name[0]=0;
      }
    } else if(m.screen==R497_MENU_LOAD) {
      if(press&(1u<<8))m.screen=R497_MENU_MAIN;
      if(save_count){
        if(press&(1u<<4))load_index=(load_index+save_count-1)%save_count;
        if(press&(1u<<5))load_index=(load_index+1)%save_count;
        if(press&(1u<<0)){
          R497_FreeStaged(&staged);bool ok=R497_ReadSave(runtime,saves[load_index],ruleset,&staged);
          bool backed=false,applied=false;
          if(ok){backed=R497_SaveBackup(runtime,&backup,*io_frame);applied=backed&&R497_ApplyStaged(runtime,&staged);}
          if(applied){
            *io_frame=staged.h.frame;
            if(clock){clock->level=staged.h.level;clock->frames=staged.h.level_frames;clock->active=staged.h.level>=1&&staged.h.level<=56;clock->exit_last=zamn_runtime_exit_door_last(runtime);clock->exit_mask=0;clock->score_recorded=false;}
            loaded=true;running=false;
          } else {
            if(backed&&backup.data&&!R497_ApplyStaged(runtime,&backup))fatal=true;
            memset(m.status,0,sizeof(m.status));strncpy(m.status,"LOAD FAILED",sizeof(m.status)-1);m.screen=R497_MENU_MAIN;
          }
          R497_FreeStaged(&staged);R497_FreeStaged(&backup);
        }
      }
    }
    m.list_count=0;m.list_selected=0;memset(m.list,0,sizeof(m.list));
    if(m.screen==R497_MENU_LOAD&&save_count){int first=load_index-(R497_SAVE_PAGE/2);if(first<0)first=0;if(first>save_count-R497_SAVE_PAGE)first=save_count-R497_SAVE_PAGE;if(first<0)first=0;int show=save_count-first;if(show>R497_SAVE_PAGE)show=R497_SAVE_PAGE;m.list_count=(uint8_t)show;m.list_selected=(uint8_t)(load_index-first);for(int i=0;i<show;++i)strncpy(m.list[i],saves[first+i],R497_NAME_MAX);}
    if(running)R497_DrawPauseMenu(runtime,last_native,&m,true,savable);
  }
  R497_FreeStaged(&staged);R497_FreeStaged(&backup);Xbox_Audio_SetMuted(false);
  if(fatal)return false;if(out_loaded)*out_loaded=loaded;return true;
}

static void R498_ResetLevelClock(void){memset(&s_r498_clock,0,sizeof(s_r498_clock));R498_PendingScoresClear();}
static uint8_t R498_RequiredExitMask(ZamnNativeRuntime* runtime,int players) {
  if(players<=1)return 1u;
  uint8_t mask=0;
  // A player whose lives word has underflowed to $FFFF is out of the run.
  // Everyone else must actually reach the exit door before a 2P time stops.
  if(zamn_runtime_player_lives(runtime,0)!=0xffffu)mask|=1u;
  if(zamn_runtime_player_lives(runtime,1)!=0xffffu)mask|=2u;
  return mask?mask:3u;
}
static bool R498_AdvanceLevelClock(ZamnNativeRuntime* runtime,uint8_t ruleset,int players,
                                   uint16_t* out_level,uint32_t* out_frames) {
  if(out_level)*out_level=0;if(out_frames)*out_frames=0;
  const uint16_t level=zamn_runtime_current_level(runtime);
  if(level<1||level>56)return false;
  const uint16_t door=zamn_runtime_exit_door_last(runtime);
  if(!s_r498_clock.active){
    s_r498_clock.level=level;s_r498_clock.frames=1;s_r498_clock.exit_last=door;
    s_r498_clock.exit_mask=0;s_r498_clock.active=true;s_r498_clock.score_recorded=false;
    return false;
  }
  if(level==s_r498_clock.level){
    if(!s_r498_clock.score_recorded&&s_r498_clock.frames!=0xffffffffu)++s_r498_clock.frames;
    if(door!=s_r498_clock.exit_last){
      s_r498_clock.exit_last=door;
      if(door==5u)s_r498_clock.exit_mask|=1u;
      else if(door==6u)s_r498_clock.exit_mask|=2u;
      const uint8_t need=R498_RequiredExitMask(runtime,players);
      if(!s_r498_clock.score_recorded&&(s_r498_clock.exit_mask&need)==need){
        s_r498_clock.score_recorded=true;
        if(out_level)*out_level=s_r498_clock.level;if(out_frames)*out_frames=s_r498_clock.frames;
        return true;
      }
    }
    return false;
  }
  const uint16_t completed=s_r498_clock.level;const uint32_t elapsed=s_r498_clock.frames;
  const bool need_fallback=!s_r498_clock.score_recorded;
  s_r498_clock.level=level;s_r498_clock.frames=1;s_r498_clock.exit_last=door;
  s_r498_clock.exit_mask=0;s_r498_clock.active=true;s_r498_clock.score_recorded=false;
  if(completed>=1&&completed<=56&&elapsed){
    // Rewards happen on the actual level boundary. Leaderboard timing normally
    // stopped earlier, on the exit-door collision; the level change is only a
    // fallback for unusual/dead-player transition paths.
    if(Xbox_Netplay_RulesetNormal(ruleset)&&(completed%5u)==0u)
      zamn_runtime_normal_checkpoint_reward(runtime,players);
    if(need_fallback){if(out_level)*out_level=completed;if(out_frames)*out_frames=elapsed;return true;}
  }
  return false;
}
#endif

#ifdef ZAMN_R48_ROLLBACK
#define R48_RB_SLOTS 6
struct R48RollbackSlot {
  uint32_t frame;
  int size;
  uint8_t* data;
  bool valid;
  R498LevelClock clock;
};
struct R48HashSlot {
  uint32_t frame;
  uint64_t hash;
  bool valid;
  bool submitted;
  bool defer_logged;
};
static R48RollbackSlot s_r48_rb[R48_RB_SLOTS];
static R48HashSlot s_r48_hash[256];
static int s_r48_state_size = 0;
static int s_r48_slot_count = 0;
static uint32_t s_r49_saves, s_r49_skips, s_r49_refreshes, s_r49_replay_saves;
static uint64_t s_r49_save_ticks, s_r49_save_bytes;

static void R48_RollbackFree(void);
static bool R48_RollbackInit(ZamnNativeRuntime* runtime, uint8_t window) {
  R48_RollbackFree();
  memset(s_r48_rb, 0, sizeof(s_r48_rb));
  s_r49_saves = s_r49_skips = s_r49_refreshes = s_r49_replay_saves = 0;
  s_r49_save_ticks = s_r49_save_bytes = 0;
  memset(s_r48_hash, 0, sizeof(s_r48_hash));
  s_r48_state_size = zamn_runtime_snapshot_size(runtime);
  s_r48_slot_count = (int)window + 2;
  if (s_r48_slot_count < 2) s_r48_slot_count = 2;
  if (s_r48_slot_count > R48_RB_SLOTS) s_r48_slot_count = R48_RB_SLOTS;
  if (s_r48_state_size <= 0) return false;
  for (int i = 0; i < s_r48_slot_count; ++i) {
    s_r48_rb[i].data = (uint8_t*)malloc((size_t)s_r48_state_size);
    if (!s_r48_rb[i].data) return false;
  }
  Xbox_Log("ZNET R49 rollback snapshots size=%d slots=%d bytes=%lu resolution=UNRESTRICTED\n",
           s_r48_state_size, s_r48_slot_count,
           (unsigned long)((uint32_t)s_r48_state_size * (uint32_t)s_r48_slot_count));
  return true;
}

static void R48_RollbackFree(void) {
  for (int i = 0; i < R48_RB_SLOTS; ++i) {
    if (s_r48_rb[i].data) free(s_r48_rb[i].data);
    s_r48_rb[i].data = NULL; s_r48_rb[i].valid = false;
  }
  s_r48_state_size = 0; s_r48_slot_count = 0;
  memset(s_r48_hash,0,sizeof(s_r48_hash));
  s_r49_saves=s_r49_skips=s_r49_refreshes=s_r49_replay_saves=0;
  s_r49_save_ticks=s_r49_save_bytes=0;
  R498_PendingScoresClear();
}

static bool R48_SaveState(ZamnNativeRuntime* runtime, uint32_t frame) {
  if (s_r48_slot_count <= 0) return false;
  R48RollbackSlot* slot = &s_r48_rb[frame % (uint32_t)s_r48_slot_count];
  // R49.2: R49 used FrameAuthoritative(old_frame) as an unconditional
  // recycle gate. After a long stretch with no snapshots, the old frame ages
  // out of the 256-frame input ring; the authority query then returns false
  // even though rollback can no longer request that ancient state. That made
  // the slot permanently unrecyclable and surfaced as SNAPSHOT_SAVE(7).
  // Only an old state still inside the negotiated rollback window needs
  // protection. Xbox_Netplay_TakeRollbackRequest() already rejects deeper
  // requests, so states older than the window are safe to recycle.
  if (slot->valid && slot->frame != frame) {
    const uint32_t age = frame - slot->frame;
    const uint32_t window = (uint32_t)Xbox_Netplay_RollbackWindow();
    const bool authoritative = Xbox_Netplay_FrameAuthoritative(slot->frame);
    if (age <= window && !authoritative) return false;
    if (!authoritative) {
      Xbox_Log("ZNET R49.2 recycle expired snapshot old=%lu new=%lu age=%lu window=%lu\n",
               (unsigned long)slot->frame, (unsigned long)frame,
               (unsigned long)age, (unsigned long)window);
    }
  }
  int size = 0;
  const uint64_t started = __builtin_readcyclecounter();
  if (!zamn_runtime_save_snapshot(runtime, slot->data, s_r48_state_size, &size)) {
    const int needed = zamn_runtime_snapshot_needed_size(runtime);
    Xbox_Log("ZNET R49 snapshot capacity failure frame=%lu needed=%d capacity=%d\n",
             (unsigned long)frame, needed, s_r48_state_size);
    return false;
  }
  s_r49_save_ticks += __builtin_readcyclecounter() - started;
  s_r49_save_bytes += (uint32_t)size; ++s_r49_saves;
  slot->frame = frame; slot->size = size; slot->valid = true;
  slot->clock = s_r498_clock;
  if (frame <= 1u)
    Xbox_Log("ZNET R49 snapshot frame=%lu bytes=%d capacity=%d\n",
             (unsigned long)frame, size, s_r48_state_size);
  return true;
}

static bool R48_LoadState(ZamnNativeRuntime* runtime, uint32_t frame) {
  if (s_r48_slot_count <= 0) return false;
  R48RollbackSlot* slot = &s_r48_rb[frame % (uint32_t)s_r48_slot_count];
  if(!slot->valid || slot->frame != frame ||
     !zamn_runtime_load_snapshot(runtime, slot->data, slot->size)) return false;
  s_r498_clock=slot->clock;
  R498_PendingScoresDiscardFrom(frame);
  return true;
}

static void R48_RecordHash(uint32_t frame, uint64_t hash) {
  R48HashSlot* h = &s_r48_hash[frame & 255u];
  const bool same_frame = h->valid && h->frame == frame;
  const bool was_deferred = same_frame && h->defer_logged;
  h->frame = frame; h->hash = hash; h->valid = true; h->submitted = false;
  h->defer_logged = was_deferred;
}

static void R48_SubmitReadyHashes(uint32_t current_frame, uint32_t window) {
  for (unsigned i = 0; i < 256u; ++i) {
    R48HashSlot* h = &s_r48_hash[i];
    if (!h->valid || h->submitted) continue;
    if (((h->frame + 1u) % 30u) != 0u) continue;
    if (current_frame < h->frame || current_frame - h->frame < window) continue;

    if (!Xbox_Netplay_FrameAuthoritative(h->frame)) {
      if (!h->defer_logged) {
        Xbox_Log("ZNET R49 HASH defer frame=%lu current=%lu waiting-authoritative-peer-input\n",
                 (unsigned long)h->frame, (unsigned long)current_frame);
        h->defer_logged = true;
      }
      continue;
    }

    Xbox_Netplay_SubmitConfirmedHash(h->frame, h->hash);
    h->submitted = true;
    if (h->defer_logged) {
      Xbox_Log("ZNET R49 HASH submit frame=%lu current=%lu authoritative=1\n",
               (unsigned long)h->frame, (unsigned long)current_frame);
    }
  }
}

static bool R48_ApplyPendingRollback(ZamnNativeRuntime* runtime, uint32_t frame,
                                     bool* did_replay) {
  if (did_replay) *did_replay = false;
  uint32_t from = 0;
  if (!Xbox_Netplay_TakeRollbackRequest(frame, &from)) return Xbox_Netplay_RollbackHealthy();

  const uint32_t depth = frame - from;
  if (!R48_LoadState(runtime, from)) {
    Xbox_Log("ERROR: R49 rollback snapshot missing frame=%lu current=%lu\n",
             (unsigned long)from, (unsigned long)frame);
    Xbox_Netplay_ReportFatal(ZNP_EXIT_SNAPSHOT_LOAD, from);
    return false;
  }

  zamn_runtime_set_replay_suppressed(runtime, true);
  bool replay_ok = true;
  for (uint32_t rf = from; rf < frame; ++rf) {
    // Refresh only still-predicted frames after an earlier correction. Frames
    // whose exact input is authoritative can always be reconstructed by replay.
    if (rf != from && !Xbox_Netplay_RemoteInputAuthoritative(rf)) {
      if (!R48_SaveState(runtime, rf)) {
        Xbox_Netplay_ReportFatal(ZNP_EXIT_SNAPSHOT_SAVE, rf);
        replay_ok = false;
        break;
      }
      ++s_r49_replay_saves;
    }
    uint16_t rp1 = 0, rp2 = 0;
    if (!Xbox_Netplay_GetReplayInputs(rf, &rp1, &rp2)) {
      Xbox_Netplay_ReportFatal(ZNP_EXIT_REPLAY_INPUT, rf);
      replay_ok = false;
      break;
    }
    zamn_runtime_set_pad(runtime, 0, (uint16_t)(rp1 & R497_PAD_GAME_MASK));
    zamn_runtime_set_pad(runtime, 1, (uint16_t)(rp2 & R497_PAD_GAME_MASK));
    ZamnFrameResult rfr;
    if (!zamn_runtime_frame(runtime, NULL, 0, &rfr)) {
      Xbox_Netplay_ReportFatal(ZNP_EXIT_REPLAY_RUNTIME, rf);
      replay_ok = false;
      break;
    }
    {
      uint16_t completed_level=0;uint32_t completed_frames=0;
      if(R498_AdvanceLevelClock(runtime,s_r498_ruleset,2,&completed_level,&completed_frames))
        R498_QueuePendingScore(s_r498_ruleset,completed_level,completed_frames,2,false,rf);
    }
    if (((rf + 1u) % 30u) == 0u)
      R48_RecordHash(rf, zamn_runtime_state_hash(runtime));
  }
  zamn_runtime_set_replay_suppressed(runtime, false);

  if (!replay_ok) {
    Xbox_Log("ERROR: R49 rollback replay failed from=%lu depth=%lu\n",
             (unsigned long)from, (unsigned long)depth);
    return false;
  }

  Xbox_Netplay_NoteReplay(depth);
  if (did_replay) *did_replay = true;
  return true;
}
#endif

#if defined(ZAMN_RELEASE_NO_DIAGNOSTICS) && !defined(ZAMN_R499_SERVICE_LOG)
static void Xbox_OpenLog(void) {}
static void Xbox_FlushLog(void) {}
#else
static const char *Xbox_LogName(void) {
#ifdef ZAMN_STOCK_CORE
  return "zamn-reference.log";
#else
  return "zamn-native.log";
#endif
}

static void Xbox_OpenLog(void) {
  if (s_log) return;
  const char *roots[] = { "D:\\", "T:\\", "E:\\" };
  const char *name = Xbox_LogName();
  for (int i = 0; i < (int)(sizeof(roots) / sizeof(roots[0])); ++i) {
    char path[64];
    snprintf(path, sizeof(path), "%s%s", roots[i], name);
    FILE *f = fopen(path, "wb");
    if (!f) continue;
    s_log = f;
#ifdef ZAMN_R39_BUFFERED_LOG
    setvbuf(s_log, s_log_buffer, _IOFBF, sizeof(s_log_buffer));
#endif
    strncpy(s_log_path, path, sizeof(s_log_path) - 1);
    s_log_path[sizeof(s_log_path) - 1] = 0;
    fprintf(s_log, "ZAMN-XBOX logger opened: %s\n", s_log_path);
#ifndef ZAMN_R39_BUFFERED_LOG
    fflush(s_log);
    DbgPrint("ZAMN-XBOX: logger opened: %s\n", s_log_path);
#endif
    break;
  }
}

extern "C" void Xbox_Log(const char *fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
  va_end(ap);
  buf[sizeof(buf) - 1] = 0;
#ifndef ZAMN_R39_BUFFERED_LOG
  DbgPrint("ZAMN-XBOX: %s", buf);
#endif
  if (!s_log) Xbox_OpenLog();
  if (s_log) {
    fputs(buf, s_log);
#ifndef ZAMN_R39_BUFFERED_LOG
    fflush(s_log);
#endif
  }
}

static void Xbox_FlushLog(void) {
  if (s_log) fflush(s_log);
}

extern "C" void Xbox_LogFlush(void) {
  Xbox_FlushLog();
}

#endif

static uint64_t HashBytes64(const uint8_t *p, int n) {
  uint64_t h = UINT64_C(1469598103934665603);
  for (int i = 0; p && i < n; ++i) h = (h ^ p[i]) * UINT64_C(1099511628211);
  return h;
}

static uint8_t *ReadWholeFile(const char *path, int *outSize) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  if (n <= 0 || n > 16 * 1024 * 1024) { fclose(f); return NULL; }
  uint8_t *p = (uint8_t*)malloc((size_t)n);
  if (!p) { fclose(f); return NULL; }
  if (fread(p, 1, (size_t)n, f) != (size_t)n) {
    fclose(f); free(p); return NULL;
  }
  fclose(f); *outSize = (int)n; return p;
}

static bool FileExists(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) return false;
  fclose(f);
  return true;
}

static uint8_t *FindRemasteredBank(int *size, const char **used) {
  static const char *paths[] = {
    "D:\\zamn-remastered.zsb",
    "D:\\Media\\zamn-remastered.zsb"
  };
  for (int i = 0; i < (int)(sizeof(paths)/sizeof(paths[0])); ++i) {
    uint8_t *p = ReadWholeFile(paths[i], size);
    if (p) { *used = paths[i]; return p; }
  }
  return NULL;
}

static uint8_t *FindRom(int *size, const char **used) {
  static const char *paths[] = {
    "D:\\Zombies Ate My Neighbors.sfc",
    "D:\\Zombies Ate My Neighbors (USA).sfc",
    "D:\\zamn.sfc",
    "D:\\Media\\Zombies Ate My Neighbors.sfc",
    "D:\\Media\\zamn.sfc"
  };
  for (int i = 0; i < (int)(sizeof(paths)/sizeof(paths[0])); ++i) {
    uint8_t *p = ReadWholeFile(paths[i], size);
    if (p) { *used = paths[i]; return p; }
  }
  return NULL;
}

int main(void) {
#ifdef ZAMN_STOCK_CORE
  Xbox_Log("boot R46 REFERENCE BACKEND\n");
#elif defined(ZAMN_R47_NETPLAY)
  Xbox_Log("boot R49.9.7 RS-ROLLBACK-TOGGLE + R49.9.6 PUBLIC-NOLOG + R49.9.5 COMPACT-BOARDS/MODE-LABELS + R49.9.4 UI/PROFILE/RECORD-DATES + R49.9.3 ASYNC-MENUS/WARM-NET/PACKED-BOARDS + R49.9.2 HARD-AUDIO-STOP/LOADING-OVERLAY/RELIABLE-BOARDS + R49.9.1 SESSION-AUTH-CACHE + R49.9 ACCOUNTS/CHAT/BEST-ONLY-BOARDS + R49.8.1 EXIT-TIMED-LEADERBOARDS + R49.8 MODES/PROFILES/SOLO + R49.7 ONLINE-SAVE + R49.6 START-AUDIO-MUTE + R49.5 INSTANT-CANONICAL-START + R49.4 LOBBY/STAGED-RENDER + RESPONSIVE-START + LIVE TITLE ONLINE + INDEPENDENT DELAY/RB + ROLLBACK SLOT RECYCLE + APU PHASE RESTORE + R49 PUBLIC ROOMS + AUTO NAT/RELAY + DIRECT PLAY + 2P DETERMINISTIC UDP LOCKSTEP\n");
#elif defined(ZAMN_R46_APU_COMPACT_TRACE)
  Xbox_Log("boot R46 COMPACT APU TRACE + STRICT NATIVE SHARE\n");
#elif defined(ZAMN_R45_APU_SET_NATIVE_TRANSFER)
  Xbox_Log("boot R45 NATIVE APU SET TRANSFER CUTOVER\n");
#elif defined(ZAMN_R44_NATIVE_BURN_ATTRIB)
  Xbox_Log("boot R44 NATIVE BURN OWNER ATTRIBUTION\n");
#elif defined(ZAMN_R43_NATIVE_WAIT_CUTOVER)
  Xbox_Log("boot R43 TRUE NATIVE NMI + TRANSITION WAIT CUTOVER\n");
#elif defined(ZAMN_R42_LEVEL_INTRO_WAIT_CUTOVER)
  Xbox_Log("boot R42.1 ENHANCED AUDIO PRESETS + LEVEL-INTRO CUTOVER\n");
#elif defined(ZAMN_R41_TRANSITION_WAIT_CUTOVER)
  Xbox_Log("boot R41.1 NATIVE PLATFORM AUDIO MODE FIX\n");
#elif defined(ZAMN_R40_FAST_OAM_PREFLIGHT)
  Xbox_Log("boot R40 TRUE NATIVE FAST OAM PREFLIGHT\n");
#elif defined(ZAMN_R39_BUFFERED_LOG)
  Xbox_Log("boot R39 LOCKED-60 BUFFERED RELEASE LOGGING\n");
#elif defined(ZAMN_R38_READONLY_WALK_GUARD)
  Xbox_Log("boot R38 TRUE NATIVE WALK GUARD CUTOVER\n");
#elif defined(ZAMN_R37_READONLY_GUARDS)
  Xbox_Log("boot R37 TRUE NATIVE READ-ONLY GUARD CUTOVER\n");
#elif defined(ZAMN_R36_NATIVE_LEAN)
  Xbox_Log("boot R36 TRUE NATIVE GAMEPLAY LEAN CUTOVER\n");
#elif defined(ZAMN_R35_NATIVE_OBJECT_PIPELINE)
  Xbox_Log("boot R35 LOCKED-60 NATIVE OBJECT PIPELINE\n");
#elif defined(ZAMN_R34_TRUE_NATIVE_CUTOVER)
  Xbox_Log("boot R34 TRUE NATIVE CUTOVER 2\n");
#else
  Xbox_Log("boot R34 R33-COMPAT BACKEND\n");
#endif
#if !defined(ZAMN_RELEASE_NO_DIAGNOSTICS) || defined(ZAMN_R499_SERVICE_LOG)
  if (s_log_path[0]) Xbox_Log("log-path=%s\n", s_log_path);
#endif

  int romSize = 0; const char *romPath = NULL;
  uint8_t *rom = FindRom(&romSize, &romPath);
  if (!rom) {
    Xbox_Log("ERROR: ROM not found. Put 'Zombies Ate My Neighbors.sfc' or 'zamn.sfc' beside default.xbe.\n");
    Xbox_FlushLog();
    for (;;) Sleep(1000);
  }
  Xbox_Log("ROM assets/reference: %s (%d bytes)\n", romPath, romSize);
  const uint64_t romHash = HashBytes64(rom, romSize);
#ifdef ZAMN_R47_NETPLAY
  Xbox_Log("r47-rom-hash=FNV64:%08lX%08lX\n", (unsigned long)(romHash >> 32), (unsigned long)romHash);
#endif

#ifdef ZAMN_STOCK_CORE
  const ZamnRuntimeMode mode = ZAMN_RUNTIME_REFERENCE;
#else
  const ZamnRuntimeMode mode = ZAMN_RUNTIME_NATIVE_CUTOVER;
#endif
  ZamnNativeRuntime *runtime = zamn_runtime_create(rom, romSize, mode);
  free(rom);
  if (!runtime) {
    Xbox_Log("ERROR: native runtime init failed\n");
    Xbox_FlushLog();
    for (;;) Sleep(1000);
  }

  uint8_t *remasteredBank = NULL;
  int remasteredBankSize = 0;
  const char *remasteredBankPath = NULL;
#ifdef ZAMN_R41_NATIVE_AUDIO
#ifndef ZAMN_R41_AUDIO_DEFAULT
#define ZAMN_R41_AUDIO_DEFAULT 0
#endif
#ifndef ZAMN_R421_AUDIO_PROFILE
#define ZAMN_R421_AUDIO_PROFILE 0
#endif
  ZamnNativeAudioMode audioMode = (ZamnNativeAudioMode)ZAMN_R41_AUDIO_DEFAULT;
  ZamnNativeAudioProfile audioProfile =
      (ZamnNativeAudioProfile)ZAMN_R421_AUDIO_PROFILE;
  // R41.1: the selected build mode is compiled into the XBE, so copying
  // default.xbe alone cannot silently fall back to Original. Legacy sidecar
  // flags are consulted only by an Original-default XBE; an explicitly built
  // Enhanced/Remastered XBE cannot be changed by stale flag files.
#if ZAMN_R41_AUDIO_DEFAULT == 0
  if (FileExists("D:\\audio_remastered.flag"))
    audioMode = ZAMN_AUDIO_REMASTERED;
  else if (FileExists("D:\\audio_enhanced.flag"))
    audioMode = ZAMN_AUDIO_ENHANCED;
#endif
  zamn_runtime_set_audio_mode(runtime, audioMode);
  zamn_runtime_set_audio_profile(runtime, audioProfile);
  if (audioMode == ZAMN_AUDIO_REMASTERED) {
    remasteredBank = FindRemasteredBank(&remasteredBankSize, &remasteredBankPath);
    if (remasteredBank && !zamn_runtime_set_remastered_bank(
                              runtime, remasteredBank, (uint32_t)remasteredBankSize)) {
      free(remasteredBank);
      remasteredBank = NULL;
      remasteredBankSize = 0;
      remasteredBankPath = NULL;
    }
  }
#endif

  Xbox_Log("runtime-owner=Xbox native runtime\n");
  Xbox_Log("backend=%s\n", zamn_runtime_backend_name(runtime));
  Xbox_Log("native-substitutions=%d\n",
           zamn_runtime_native_substitutions(runtime));
#if defined(ZAMN_R70_HOT_THREAD_9A6D) && defined(ZAMN_R39_BUFFERED_LOG) && !defined(ZAMN_RELEASE_NO_DIAGNOSTICS)
  // R69's last console log reported 390 while the compiled cumulative
  // registry with all 47 release feature flags contains 393 unique PCs.
  // Alert on an out-of-sync/stale or differently configured XBE at startup.
  Xbox_Log("R70REGISTRY expected=393 actual=%d status=%s\n",
           zamn_runtime_native_substitutions(runtime),
           zamn_runtime_native_substitutions(runtime)==393 ? "OK" : "MISMATCH-CHECK-BUILD-DEFINES");
#endif
#ifdef ZAMN_R41_NATIVE_AUDIO
  Xbox_Log("audio-mode=%s\n", zamn_runtime_audio_mode_name(runtime));
  Xbox_Log("audio-build-default=%d\n", (int)ZAMN_R41_AUDIO_DEFAULT);
  if (audioMode != ZAMN_AUDIO_ORIGINAL) {
    Xbox_Log("audio-preset=%s\n", zamn_runtime_audio_profile_name(runtime));
    Xbox_Log("audio-preset-default=%d\n", (int)ZAMN_R421_AUDIO_PROFILE);
  }
  if (audioMode == ZAMN_AUDIO_REMASTERED) {
    if (zamn_runtime_has_remastered_bank(runtime))
      Xbox_Log("remastered-bank=%s (%d bytes)\n", remasteredBankPath, remasteredBankSize);
    else
      Xbox_Log("remastered-bank=not found/invalid; missing source IDs fall back to original BRR\n");
  }
#endif
#ifndef ZAMN_STOCK_CORE
  Xbox_Log("cutover=frontend no longer owns Snes/Ppu/Cosim; translated C is primary; untranslated regions fall back behind runtime boundary\n");
  Xbox_Log("dispatch=O(1) PC->native hash; linear registry scan removed from Xbox native fallback\n");
#ifdef ZAMN_R36_NATIVE_LEAN
#ifdef ZAMN_R291_HOT_RESIDUE
  Xbox_Log("release-lean=full per-opcode accounting disabled; low-rate residual PC sampler enabled; native pricing scaffolding stripped\n");
#else
  Xbox_Log("release-lean=full per-opcode accounting and residual sampling disabled; native pricing scaffolding stripped\n");
#endif
#else
  Xbox_Log("release-lean=per-opcode call accounting/profiling disabled; cycle accounting retained\n");
#endif
#ifdef ZAMN_R34_TRUE_NATIVE_CUTOVER
  Xbox_Log("r34-cutover=ordinary translated gameplay/scheduler C commits immediately; fake 65816 burn removed\n");
  Xbox_Log("r34-chain=native returns and jump exits chain directly; hardware-timed PPU/DMA/APU routines remain compatibility-timed\n");
  Xbox_Log("r34-idle=WAI fast-forward to scanline boundaries when timer IRQs are disabled\n");
#endif
#ifdef ZAMN_R35_NATIVE_OBJECT_PIPELINE
  Xbox_Log("r35-objects=OAM scanline lists cached per OAM epoch; no 128-sprite rescan on every visible line\n");
  Xbox_Log("r35-object-mmx=fully visible normal-palette object slivers compose 8 pixels at once; flipped rows are predecoded\n");
#endif
#ifdef ZAMN_R36_NATIVE_LEAN
  Xbox_Log("r36-native-lean=ordinary native gameplay skips obsolete 65816 cycle-pricing and delayed commit snapshots\n");
#ifdef ZAMN_R291_HOT_RESIDUE
  Xbox_Log("r36-residual=low-rate executed-ROM PC sampler identifies the next blocks to native-port\n");
#else
  Xbox_Log("r36-residual=disabled in clean Release timing build\n");
#endif
  Xbox_Log("r36-guard=128KB supported-routine dry-run copies are measured for the next cutover\n");
#endif
#ifdef ZAMN_R37_READONLY_GUARDS
  Xbox_Log("r37-guards=pure supported predicates run directly on live WRAM; 128KB sandbox copies reserved for mutating guards\n");
#endif
#ifdef ZAMN_R38_READONLY_WALK_GUARD
  Xbox_Log("r38-walk-guard=player_walk support is an exact read-only predicate; one full 128KB dry-run per gameplay frame removed\n");
#ifdef ZAMN_R291_HOT_RESIDUE
  Xbox_Log("r38-residual-profiler=ON (diagnostic build)\n");
#else
  Xbox_Log("r38-residual-profiler=OFF for release timing margin\n");
#endif
#ifdef ZAMN_R39_BUFFERED_LOG
  Xbox_Log("r39-log=1MiB fully buffered file log; per-line fflush/DbgPrint removed from gameplay\n");
  Xbox_Log("r39-perf=window timer restarts after diagnostics so logger time cannot contaminate the next FPS sample\n");
#endif
#ifdef ZAMN_R40_FAST_OAM_PREFLIGHT
  Xbox_Log("r40-oam-guard=safety preflight runs sort+cull+metasprite validation+collision dispatch only; duplicate OAM emission removed\n");
  Xbox_Log("r40-guard-copy=sprite preflight clones only bank $7E (64 KiB) instead of full 128 KiB WRAM\n");
#endif
#ifdef ZAMN_R41_TRANSITION_WAIT_CUTOVER
  Xbox_Log("r41-waits=known VBlank/VRAM busy-wait ROM loops advance native machine events by scanline instead of interpreting the empty loop\n");
#endif
#ifdef ZAMN_R42_LEVEL_INTRO_WAIT_CUTOVER
  Xbox_Log("r42-intro-waits=verified $82:AC65/$82:AC92 LDA/CMP/BCC level-intro polls advance native machine events instead of interpreting the empty loop\n");
  Xbox_Log("r42-safety=optimization requires exact ROM opcode pattern, 16-bit A, loop-back target, and WRAM operand; final loop iteration executes normally\n");
#endif
#ifdef ZAMN_R43_NATIVE_WAIT_CUTOVER
  Xbox_Log("r43-autojoy=$80:81B5 SNES $4212 auto-joy poll fast-forwards LakeSnes autoJoyTimer; final <=64 clocks and exit execute normally\n");
  Xbox_Log("r43-poll=$80:933A candidate accelerates only after exact side-effect-free LDA/CMP/branch-back ROM pattern verification\n");
#endif
#ifdef ZAMN_R45_APU_SET_NATIVE_TRANSFER
  Xbox_Log("r45-apu-set=$80:CC7C keeps SPC700 writes/acks machine-timed but removes translated 65816 instruction runs between them\n");
#endif
#ifdef ZAMN_R46_APU_COMPACT_TRACE
  Xbox_Log("r46-apu-trace=$80:CC7C suppresses CPU-only HW_RUN construction at source and coalesces zero-time stack states; SPC waits/writes remain exact events\n");
#endif
#ifdef ZAMN_R46_STRICT_NATIVE_SHARE
  Xbox_Log("r46-share=window native-equivalent share restores R34/R46 elided CPU work; strict-cpu excludes native-started DMA\n");
#endif
#ifdef ZAMN_R47_NETPLAY
  Xbox_Log("r47.11-netplay=OFFLINE/HOST DIRECT/JOIN DIRECT/PUBLIC ROOMS; direct play UDP/6464; public directory+NAT+relay UDP/6467; AUTO/manual 0..12 retained\n");
  Xbox_Log("r47.11-public=public rooms register/list through directory; gameplay/NAT uses local UDP/6464; same-WAN peers receive private-LAN candidates; relay remains fallback\n");
  Xbox_Log("r47.11-direct=direct host keeps MK64-style public-IP discovery; direct join keeps visible boxed IPv4 cursor\n");
  Xbox_Log("r47.11-sync=host=P1 join=P2 in both Direct and Public Rooms; host presses A to start; simulation never advances without both logical pads\n");
  Xbox_Log("r47.11-hash=explicit WRAM+65816/timing-state FNV64 every 30 logical frames; contradictory input/hash is fail-stop\n");
  Xbox_Log("r49.1-rb-apu-phase=apuMasterPending+apuCycleDebtNumerator restored by rollback snapshot and covered by state hash\n");
  Xbox_Log("r49.2-rb-slot-recycle=expired snapshots may recycle after leaving retained rollback ring; stale input-history authority no longer causes SNAPSHOT_SAVE(7)\n");
  Xbox_Log("r49.5-start-cache=verified canonical title snapshot; one-frame START restore; cold 2224-frame prepare retained only as cache-miss fallback\n");
  Xbox_Log("r49.6-start-audio=DirectSound muted+cleared from queued START through BOOT_GO; fresh gameplay PCM resumes after release\n");
  Xbox_Log("r49.7-online-save=START+BACK shared pause; named synchronized snapshots; PSO-style compact keyboard\n");
#endif
#ifdef ZAMN_R41_NATIVE_RENDER_OWNER
  Xbox_Log("r41-render=Xbox native runtime owns Mode-1 line composition; generic PPU remains exact fallback for unsupported raster states\n");
#endif
#ifdef ZAMN_R41_NATIVE_AUDIO
  Xbox_Log("r41-audio=native Xbox mastering + optional PCM source replacement bridge; Original mode remains unchanged\n");
#ifdef ZAMN_R421_ENHANCED_PRESETS
  Xbox_Log("r42.1-audio-presets=Balanced (R41 reference), Punchy (centre/impact), Wide (stereo-space)\n");
#endif
#endif
#endif
#endif

  if (FAILED(Xbox_D3D_Init())) {
    Xbox_Log("ERROR: D3D init failed\n");
    Xbox_FlushLog();
    for (;;) Sleep(1000);
  }
  bool audio_ok = SUCCEEDED(Xbox_Audio_Init());
  Xbox_Input_Init();
#ifdef ZAMN_R47_NETPLAY
  if (!Xbox_Netplay_Init(romHash)) {
    Xbox_Log("ERROR: R49 netplay initialization failed\n");
    Xbox_FlushLog();
    for (;;) Sleep(1000);
  }
  s_title_runtime=runtime;s_title_rom_hash=romHash;s_title_menu.Reset();
  TitleCacheRead();
  Xbox_Netplay_SetFrontend(TitleBackground,TitlePrepareMatch);
#endif
#ifdef ZAMN_R47_NETPLAY
  Xbox_Log("audio=%s; entering R49 runtime loop\n", audio_ok ? "OK" : "FAILED");
#elif defined(ZAMN_R46_APU_COMPACT_TRACE)
  Xbox_Log("audio=%s; entering R46 runtime loop\n", audio_ok ? "OK" : "FAILED");
#elif defined(ZAMN_R45_APU_SET_NATIVE_TRANSFER)
  Xbox_Log("audio=%s; entering R45 runtime loop\n", audio_ok ? "OK" : "FAILED");
#elif defined(ZAMN_R44_NATIVE_BURN_ATTRIB)
  Xbox_Log("audio=%s; entering R44 runtime loop\n", audio_ok ? "OK" : "FAILED");
#elif defined(ZAMN_R43_NATIVE_WAIT_CUTOVER)
  Xbox_Log("audio=%s; entering R43 runtime loop\n", audio_ok ? "OK" : "FAILED");
#elif defined(ZAMN_R42_LEVEL_INTRO_WAIT_CUTOVER)
  Xbox_Log("audio=%s; entering R42 runtime loop\n", audio_ok ? "OK" : "FAILED");
#elif defined(ZAMN_R41_TRANSITION_WAIT_CUTOVER)
  Xbox_Log("audio=%s; entering R41 runtime loop\n", audio_ok ? "OK" : "FAILED");
#elif defined(ZAMN_R40_FAST_OAM_PREFLIGHT)
  Xbox_Log("audio=%s; entering R40 runtime loop\n", audio_ok ? "OK" : "FAILED");
#elif defined(ZAMN_R39_BUFFERED_LOG)
  Xbox_Log("audio=%s; entering R39 runtime loop\n", audio_ok ? "OK" : "FAILED");
#elif defined(ZAMN_R38_READONLY_WALK_GUARD)
  Xbox_Log("audio=%s; entering R38 runtime loop\n", audio_ok ? "OK" : "FAILED");
#elif defined(ZAMN_R36_NATIVE_LEAN)
  Xbox_Log("audio=%s; entering R36 runtime loop\n", audio_ok ? "OK" : "FAILED");
#elif defined(ZAMN_R35_NATIVE_OBJECT_PIPELINE)
  Xbox_Log("audio=%s; entering R35 runtime loop\n", audio_ok ? "OK" : "FAILED");
#else
  Xbox_Log("audio=%s; entering R34 runtime loop\n", audio_ok ? "OK" : "FAILED");
#endif
  // Flush the boot record once.  R39 does no synchronous log flushes after
  // gameplay begins; fclose() flushes the buffered diagnostics on clean exit.
  Xbox_FlushLog();

  int16_t audio[ZAMN_AUDIO_SAMPLES_PER_FRAME * 2];
  unsigned long frame = 0;
#ifndef ZAMN_RELEASE_NO_DIAGNOSTICS
  DWORD perf_start = GetTickCount();
  DWORD input_ms = 0, core_ms = 0, video_ms = 0, audio_ms = 0, present_ms = 0;
  uint64_t core_ticks = 0;
  uint64_t machine_cycles = 0, native_cycles = 0, native_calls = 0;
  const unsigned long PERF_WINDOW = 30;
  Xbox_Log("perf-window=%lu\n", PERF_WINDOW);
#endif
#ifdef ZAMN_R46_APU_COMPACT_TRACE
  Xbox_Log("core-opt24=r46-compact-apu-trace+strict-share+r45-apu-set-native+r44-burn-owner+r43-native-waits+r42-intro+r41-platform+r40-fast-oam+r39-log+r38-walk+r37-guards+r36-lean+r35-objects+r34-scheduler\n");
#elif defined(ZAMN_R45_APU_SET_NATIVE_TRANSFER)
  Xbox_Log("core-opt23=r45-apu-set-native+r44-burn-owner+r43-native-waits+r42-intro+r41-platform+r40-fast-oam+r39-log+r38-walk+r37-guards+r36-lean+r35-objects+r34-scheduler\n");
#elif defined(ZAMN_R44_NATIVE_BURN_ATTRIB)
  Xbox_Log("core-opt22=r44-burn-owner+r43-native-waits+r42-intro+r41-platform+r40-fast-oam+r39-log+r38-walk+r37-guards+r36-lean+r35-objects+r34-scheduler\n");
#elif defined(ZAMN_R43_NATIVE_WAIT_CUTOVER)
  Xbox_Log("core-opt21=r43-native-waits+r42-intro+r41-platform+r40-fast-oam+r39-log+r38-walk+r37-guards+r36-lean+r35-objects+r34-scheduler\n");
#elif defined(ZAMN_R42_LEVEL_INTRO_WAIT_CUTOVER)
  Xbox_Log("core-opt20=r42-level-intro-waits+r41-platform+r40-fast-oam+r39-log+r38-walk+r37-guards+r36-lean+r35-objects+r34-scheduler\n");
#elif defined(ZAMN_R41_TRANSITION_WAIT_CUTOVER)
  Xbox_Log("core-opt20=r41-transition-waits+native-render-owner+native-audio-bridge+r40-fast-oam+r39-log+r38-walk+r37-guards+r36-lean+r35-objects+r34-scheduler\n");
#elif defined(ZAMN_R40_FAST_OAM_PREFLIGHT)
  Xbox_Log("core-opt20=r40-fast-oam-preflight+r39-buffered-log+r38-walk+r37-guards+r36-lean+r35-objects+r34-scheduler\n");
#elif defined(ZAMN_R39_BUFFERED_LOG)
  Xbox_Log("core-opt20=r39-buffered-log+r38-readonly-walk+r37-guards+r36-native-lean+r35-objects+r34-scheduler\n");
#elif defined(ZAMN_R38_READONLY_WALK_GUARD)
  Xbox_Log("core-opt20=r38-readonly-walk+r37-guards+r36-native-lean+r35-objects+r34-scheduler\n");
#elif defined(ZAMN_R37_READONLY_GUARDS)
  Xbox_Log("core-opt20=r37-readonly-guards+r36-native-lean+r35-objects+r34-scheduler\n");
#elif defined(ZAMN_R36_NATIVE_LEAN)
  Xbox_Log("core-opt20=r36-native-gameplay-lean+r35-objects+r34-scheduler\n");
#elif defined(ZAMN_R35_NATIVE_OBJECT_PIPELINE)
  Xbox_Log("core-opt20=r35-native-object-cache+mmx+r34-scheduler+r32-bg\n");
#elif defined(ZAMN_R34_TRUE_NATIVE_CUTOVER)
  Xbox_Log("core-opt20=r34-immediate-native+scheduler-chain+r32-ppu\n");
#else
  Xbox_Log("core-opt20=r33-compat-native-owner+direct-dispatch+r32-ppu\n");
#endif
#ifdef ZAMN_R15_THINLTO
  Xbox_Log("thinlto=ON\n");
#else
  Xbox_Log("thinlto=OFF\n");
#endif
#ifndef ZAMN_RELEASE_NO_DIAGNOSTICS
  zamn_runtime_perf_reset(runtime);
  unsigned long native256_frames = 0, fallback_frames = 0;
#endif

  bool session_return=false;
#ifdef ZAMN_R47_NETPLAY
  bool r497_last_native_video=true;
  int r497_pause_cooldown=0;
#endif
  while (1) {
#ifdef ZAMN_R47_NETPLAY
    if(session_return) {
      // Persist buffered diagnostics when leaving gameplay, before title reset.
      Xbox_FlushLog();
      // R49.9.1: silence the looping DirectSound hardware buffer for the entire
      // network teardown + canonical-title restore. A cold first-run title
      // cache seed may still take time, but it is now silent instead of
      // repeating the last gameplay sample.
      R4992_ShowLoadingOverlay();
      Xbox_Audio_SetMuted(true);
      Xbox_Log("ZNET R49.9.2 return-to-title loading overlay + audio hard-stop\n");
      Xbox_Netplay_Shutdown();
#ifdef ZAMN_R48_ROLLBACK
      R48_RollbackFree();
#endif
      s_title_prepared=false;
      if(!TitleRestoreCanonical()){Xbox_Audio_SetMuted(false);break;}
      s_title_menu.Reset(Xbox_Input_GetJoypad(0));frame=0;session_return=false;
      Xbox_Audio_SetMuted(false);
      Xbox_Log("ZNET R49.9.2 return-to-title ready\n");
      s_r498_solo_session=false;R498_ResetLevelClock();
      r497_pause_cooldown=0;
#ifndef ZAMN_RELEASE_NO_DIAGNOSTICS
      perf_start=GetTickCount();input_ms=core_ms=video_ms=audio_ms=present_ms=0;
      core_ticks=machine_cycles=native_cycles=native_calls=0;
#endif
    }
#endif
#ifndef ZAMN_RELEASE_NO_DIAGNOSTICS
    DWORD ti0 = GetTickCount();
#endif
#ifdef ZAMN_R48_ROLLBACK
    bool r48_snapshot_saved_before_poll = false;
#endif
#ifdef ZAMN_R47_NETPLAY
    if (Xbox_Netplay_Active()) {
      Xbox_Netplay_PaceBeforeInput();
      if (!Xbox_Netplay_PrepareFrame((uint32_t)frame)) {
        Xbox_Log("ERROR: R49 netplay stopped preparing frame %lu\n", frame);
        Xbox_Netplay_ReportFatal(ZNP_EXIT_PREPARE, (uint32_t)frame);
        Xbox_FlushLog();
        session_return=true; continue;
      }
#ifdef ZAMN_R48_ROLLBACK
      if (Xbox_Netplay_RollbackEnabled()) {
        bool pre_replay = false;
        if (!R48_ApplyPendingRollback(runtime, (uint32_t)frame, &pre_replay)) {
          Xbox_FlushLog();
          session_return=true; continue;
        }
        // Keep serialization before controller polling, but only when the
        // exact remote input is absent. BeginFrame may make it authoritative;
        // that harmless extra save is counted rather than risking stale input.
        if (!Xbox_Netplay_RemoteInputAuthoritative((uint32_t)frame)) {
          if (!R48_SaveState(runtime, (uint32_t)frame)) {
            Xbox_Log("ERROR: R49 snapshot save failed frame=%lu\n", frame);
            Xbox_Netplay_ReportFatal(ZNP_EXIT_SNAPSHOT_SAVE, (uint32_t)frame);
            Xbox_FlushLog(); session_return=true; continue;
          }
          r48_snapshot_saved_before_poll = true;
        }
      }
#endif
    }
#endif
    // Poll only after any lockstep/rollback catch-up wait and routine rollback
    // snapshot work so the physical sample is as fresh as possible.
    Xbox_Input_Poll();
    if (!Xbox_Input_ExitRequested()) s_exit_latched=false;
    if (Xbox_Input_ExitRequested() && !s_exit_latched) {
      s_exit_latched=true;
#ifdef ZAMN_R47_NETPLAY
      if(Xbox_Netplay_Active()){session_return=true;continue;}
#endif
      break;
    }
#ifdef ZAMN_R47_NETPLAY
    if (Xbox_Netplay_Active()) {
      uint16_t p1 = 0, p2 = 0;
      // Each console uses its first physical controller. Host owns logical P1;
      // joiner owns logical P2. The lockstep layer supplies both logical pads.
      if (!Xbox_Netplay_BeginFrame((uint32_t)frame, Xbox_Input_GetGameplayJoypad(0), &p1, &p2)) {
        Xbox_Log("ERROR: R49 netplay stopped before frame %lu\n", frame);
        Xbox_Netplay_ReportFatal(ZNP_EXIT_BEGIN, (uint32_t)frame);
        Xbox_FlushLog();
        session_return=true; continue;
      }
#ifdef ZAMN_R48_ROLLBACK
      if (Xbox_Netplay_RollbackEnabled()) {
        // BeginFrame drains the socket after the pre-poll snapshot.  If that
        // drain discovers a correction, replay it now and overwrite the saved
        // current-frame slot.  The common no-correction path performs no
        // snapshot work after the fresh controller sample.
        bool post_replay = false;
        if (!R48_ApplyPendingRollback(runtime, (uint32_t)frame, &post_replay)) {
          Xbox_FlushLog();
          session_return=true; continue;
        }
        if (!Xbox_Netplay_RemoteInputAuthoritative((uint32_t)frame) &&
            (post_replay || !r48_snapshot_saved_before_poll)) {
          if (post_replay && r48_snapshot_saved_before_poll) ++s_r49_refreshes;
          if (!R48_SaveState(runtime, (uint32_t)frame)) {
            Xbox_Log("ERROR: R49 snapshot refresh failed frame=%lu\n", frame);
            Xbox_Netplay_ReportFatal(ZNP_EXIT_SNAPSHOT_SAVE, (uint32_t)frame);
            Xbox_FlushLog();
            session_return=true; continue;
          }
        }
      }
#endif
#ifdef ZAMN_R48_ROLLBACK
      if (Xbox_Netplay_RollbackEnabled() && !r48_snapshot_saved_before_poll &&
          Xbox_Netplay_RemoteInputAuthoritative((uint32_t)frame)) ++s_r49_skips;
#endif
      // R49.7: the out-of-band START+BACK chord travels through the same
      // deterministic input stream, so both peers enter this menu on the exact
      // same logical frame.  The owner is whichever logical player pressed it.
      const bool exit_p1=(p1&R498_PAD_SYS_EXIT)!=0;
      const bool exit_p2=(p2&R498_PAD_SYS_EXIT)!=0;
      if(exit_p1||exit_p2){session_return=true;continue;}
      const bool pause_p1=(p1&R497_PAD_SYS_PAUSE)!=0;
      const bool pause_p2=(p2&R497_PAD_SYS_PAUSE)!=0;
      if(r497_pause_cooldown>0)--r497_pause_cooldown;
      if((pause_p1||pause_p2) && r497_pause_cooldown==0) {
        const uint8_t owner=(uint8_t)(pause_p1?0:1);
        const uint32_t before=(uint32_t)frame;
        uint32_t menu_frame=(uint32_t)frame;bool menu_loaded=false;
        if(!R497_RunOnlineSaveMenu(runtime,&menu_frame,owner,r497_last_native_video,
                                   s_r498_ruleset,&s_r498_clock,&menu_loaded)) {
          Xbox_Netplay_ReportFatal(ZNP_EXIT_CORE,(uint32_t)frame);
          session_return=true;continue;
        }
        frame=(unsigned long)menu_frame;
        r497_pause_cooldown=15;
        // A successful LOAD rebases the netplay timeline. Start the restored
        // frame through the normal Prepare/Begin pipeline rather than using
        // input values captured before the load.
        if(menu_loaded || (uint32_t)frame!=before)continue;
      }
      zamn_runtime_set_pad(runtime, 0, (uint16_t)(p1 & R497_PAD_GAME_MASK));
      zamn_runtime_set_pad(runtime, 1, (uint16_t)(p2 & R497_PAD_GAME_MASK));
    } else
#endif
    {
      // R78: preserve native menu controls, then use the gameplay pad for the
      // released session. R77's HUD gate never opened in the console log and
      // leaked the ROM menu's A-to-X alias and trigger radar bits into play.
      const bool frontend=!s_r498_solo_session && !s_title_menu.released;
      const uint16_t stage_level=zamn_runtime_current_level(runtime);
      const bool stage_hud=zamn_runtime_player_stage_hud(runtime);
      const bool playing=Xbox_Input_StageIsPlayable(stage_level,stage_hud);
      const bool game_pad=!frontend;
      uint16_t p1=Xbox_Input_SessionPad(frontend,Xbox_Input_GetJoypad(0),Xbox_Input_GetGameplayJoypad(0));
      uint16_t p2=Xbox_Input_SessionPad(frontend,Xbox_Input_GetJoypad(1),Xbox_Input_GetGameplayJoypad(1));
#if defined(ZAMN_R75_PAD_ROUTE_TRACE) && !defined(ZAMN_RELEASE_NO_DIAGNOSTICS)
      // Diagnostic build only: report stage transitions even if the player
      // hasn't pressed a button yet; keep per-button reports bounded.
      {
        static uint16_t priorRaw=0;
        static unsigned priorState=0xffffffffu;
        static unsigned reports=0;
        const uint16_t raw=Xbox_Input_GetJoypad(0);
        const uint16_t watched=(uint16_t)((raw&((1u<<0)|(1u<<1)|(1u<<10)|(1u<<11)|(1u<<12)|(1u<<13))) | ((Xbox_Input_ManualButtons(0)&32u)?(1u<<14):0u));
        const unsigned state=(frontend?1u:0u)|(playing?2u:0u)|(game_pad?4u:0u);
        if(state!=priorState) {
          Xbox_Log("R79STATE frontend=%u gamepad=%u ready=%u level=%u hud=%u released=%u\n",
            (unsigned)frontend,(unsigned)game_pad,(unsigned)zamn_runtime_title_ready(runtime),
            (unsigned)stage_level,(unsigned)stage_hud,(unsigned)s_title_menu.released);
          priorState=state;
        }
        if(watched!=priorRaw && reports<160) {
          Xbox_Log("R79PAD frontend=%u playing=%u raw=%04X menu=%04X mapped=%04X sent=%04X black=%u\n",
                   (unsigned)(frontend?1:0), (unsigned)(game_pad?1:0),
                   (unsigned)raw,(unsigned)Xbox_Input_GetMenuJoypad(0),
                   (unsigned)Xbox_Input_GetGameplayJoypad(0), (unsigned)p1,
                   (unsigned)((Xbox_Input_ManualButtons(0)&32u)!=0));
          ++reports;
        }
        priorRaw=watched;
      }
#endif
#ifdef ZAMN_R47_NETPLAY
      if(s_r498_solo_session) {
        if(r497_pause_cooldown>0)--r497_pause_cooldown;
        if(p1&R498_PAD_SYS_EXIT) {
          s_r498_solo_session=false;R498_ResetLevelClock();frame=0;
          R4992_ShowLoadingOverlay();
          Xbox_Audio_SetMuted(true);
          Xbox_Log("ZNET R49.9.2 solo return-to-title loading overlay + audio hard-stop\n");
          s_title_prepared=false;
          if(!TitleRestoreCanonical()){Xbox_Audio_SetMuted(false);break;}
          s_title_menu.Reset(Xbox_Input_GetJoypad(0));
          Xbox_Audio_SetMuted(false);
          Xbox_Log("ZNET R49.9.2 solo return-to-title ready\n");
          continue;
        }
        if((p1&R497_PAD_SYS_PAUSE) && r497_pause_cooldown==0) {
          uint32_t menu_frame=(uint32_t)frame;bool loaded=false;
          if(!R498_RunSoloSaveMenu(runtime,&menu_frame,r497_last_native_video,
                                   s_r498_ruleset,&s_r498_clock,&loaded))break;
          frame=(unsigned long)menu_frame;r497_pause_cooldown=15;
          if(loaded)continue;
          p1=p2=0;
        }
      } else {
        // A still-running title yield can coexist with active level actors.
        // Never reset the frontend merely because that yield was observed.
        int action=s_title_menu.Input(zamn_runtime_title_ready(runtime) && !playing,p1);
        if(action==9) {
          // Title-only local reader: no game frames or netplay inputs advance.
          R71_ShowGameManual();
          s_title_menu.previous=Xbox_Input_GetJoypad(0);
          p1=p2=0;
#ifndef ZAMN_RELEASE_NO_DIAGNOSTICS
          perf_start=GetTickCount();input_ms=core_ms=video_ms=audio_ms=present_ms=0;
          core_ticks=machine_cycles=native_cycles=native_calls=0;
          native256_frames=fallback_frames=0;zamn_runtime_perf_reset(runtime);
          ti0=GetTickCount();
#endif
        }
        if(action==8) {
          action=Xbox_Netplay_DirectMenu();
          s_title_menu.previous=Xbox_Input_GetJoypad(0);p1=p2=0;
        }
        if(action>=1 && action<=3) {
          if(!Xbox_Netplay_EnsureProfile()) {
            s_title_menu.previous=Xbox_Input_GetJoypad(0);p1=p2=0;continue;
          }
          const bool connected=Xbox_Netplay_Open(action);
          if(s_title_prepared) {
            Xbox_Audio_SetMuted(false);
            Xbox_Log("ZNET R49.6 start audio resumed connected=%u\n",(unsigned)(connected?1u:0u));
          }
          if(!connected)frame=0;
#ifndef ZAMN_RELEASE_NO_DIAGNOSTICS
          perf_start=GetTickCount();input_ms=core_ms=video_ms=audio_ms=present_ms=0;
          core_ticks=machine_cycles=native_cycles=native_calls=0;
          native256_frames=fallback_frames=0;zamn_runtime_perf_reset(runtime);
          ti0=GetTickCount();
#endif
          s_title_menu.previous=Xbox_Input_GetJoypad(0);
          if(connected) {
            s_r498_solo_session=false;s_r498_ruleset=Xbox_Netplay_Ruleset();R498_ResetLevelClock();
            frame=0;s_title_menu.Reset(Xbox_Input_GetJoypad(0));
#ifdef ZAMN_R48_ROLLBACK
            if(Xbox_Netplay_RollbackEnabled() && !R48_RollbackInit(runtime,Xbox_Netplay_RollbackWindow())) {
              Xbox_Netplay_ReportFatal(ZNP_EXIT_SNAPSHOT_INIT,0);session_return=true;
            }
#endif
            continue;
          }
          if(s_title_prepared) {
            s_title_prepared=false;
            if(!TitleRestoreCanonical())break;
          }
          p1=p2=0;
        }
        if(action==5) {
          if(!Xbox_Netplay_EnsureProfile()) { s_title_menu.previous=Xbox_Input_GetJoypad(0);p1=p2=0;continue; }
          const int mode=Xbox_Netplay_SoloMenu();
          s_title_menu.previous=Xbox_Input_GetJoypad(0);
          if(mode>=0) {
            s_r498_solo_session=true;s_r498_ruleset=(uint8_t)mode;Xbox_Netplay_SetRuleset((uint8_t)mode);R498_ResetLevelClock();
            s_title_menu.online=false;s_title_menu.visible=false;s_title_menu.released=true;
            zamn_runtime_title_control(runtime,false,0);p1=1u<<3;p2=0;frame=0;
          } else p1=p2=0;
        } else if(action==7) {
          if(Xbox_Netplay_EnsureProfile()) Xbox_Netplay_LeaderboardMenu();
          s_title_menu.previous=Xbox_Input_GetJoypad(0);p1=p2=0;
        } else if(action==6) {
          Xbox_Netplay_ProfileMenu();s_title_menu.previous=Xbox_Input_GetJoypad(0);p1=p2=0;
        } else if(action==0 || action==4) {
          zamn_runtime_title_control(runtime,false,action==4?1:0);
          p1=1u<<3;p2=0;
        } else if(s_title_menu.visible) {
          zamn_runtime_title_control(runtime,true,-1);p1=p2=0;
        }
      }
#endif
      zamn_runtime_set_pad(runtime,0,(uint16_t)(p1&R497_PAD_GAME_MASK));
      zamn_runtime_set_pad(runtime,1,(uint16_t)(p2&R497_PAD_GAME_MASK));
    }
#ifndef ZAMN_RELEASE_NO_DIAGNOSTICS
    DWORD t0 = GetTickCount();
    input_ms += t0 - ti0;
#endif

    ZamnFrameResult fr;
#ifndef ZAMN_RELEASE_NO_DIAGNOSTICS
    const uint64_t c0 = __builtin_readcyclecounter();
#endif
    if (!zamn_runtime_frame(runtime, s_native_stage, 256 * 4, &fr)) {
      Xbox_Log("ERROR: runtime frame failed at frame %lu\n", frame + 1);
#ifdef ZAMN_R47_NETPLAY
      if (Xbox_Netplay_Active()) Xbox_Netplay_ReportFatal(ZNP_EXIT_RUNTIME, (uint32_t)frame);
#endif
      Xbox_FlushLog();
      session_return=true; continue;
    }
#ifndef ZAMN_RELEASE_NO_DIAGNOSTICS
    const uint64_t c1 = __builtin_readcyclecounter();
#endif
#ifdef ZAMN_R47_NETPLAY
    r497_last_native_video=fr.native_video;
    if (Xbox_Netplay_Active()) Xbox_Netplay_MidFrame((uint32_t)frame);
    if(Xbox_Netplay_Active()||s_r498_solo_session) {
      uint16_t completed_level=0;uint32_t completed_frames=0;
      const int players=Xbox_Netplay_Active()?2:1;
      if(R498_AdvanceLevelClock(runtime,s_r498_ruleset,players,&completed_level,&completed_frames)){
        if(Xbox_Netplay_Active()&&Xbox_Netplay_RollbackEnabled())
          R498_QueuePendingScore(s_r498_ruleset,completed_level,completed_frames,(uint8_t)players,false,(uint32_t)frame);
        else
          Xbox_Netplay_RecordLevelTime(s_r498_ruleset,completed_level,completed_frames,(uint8_t)players,s_r498_solo_session);
      }
    }
#endif
#ifndef ZAMN_RELEASE_NO_DIAGNOSTICS
    DWORD t1 = GetTickCount();
    core_ticks += c1 - c0;
    machine_cycles += fr.machine_cycles;
    native_cycles += fr.native_cycles;
    native_calls += fr.native_calls;
#endif

    uint8_t *pixels = NULL; int pitch = 0;
    Xbox_D3D_BeginDraw(&pixels, &pitch);
    if (fr.native_video && pixels && pitch >= fr.width * 4) {
      const int rowBytes = fr.width * 4;
      for (int y = 0; y < fr.height; ++y)
        memcpy(pixels + y * pitch, s_native_stage + y * (256 * 4), rowBytes);
      Xbox_D3D_SetSourceRect(0, 0, fr.width, fr.height);
#ifndef ZAMN_RELEASE_NO_DIAGNOSTICS
      ++native256_frames;
#endif
    } else {
      if (pixels) {
        zamn_runtime_copy_legacy_frame(runtime, pixels, pitch, s_legacy_stage,
                                       ZAMN_FB_W * 4, ZAMN_FB_H);
      }
      Xbox_D3D_SetSourceRect(0, 16, 512, 448);
#ifndef ZAMN_RELEASE_NO_DIAGNOSTICS
      ++fallback_frames;
#endif
    }
#ifdef ZAMN_R47_NETPLAY
    if(!Xbox_Netplay_Active()&&!s_r498_solo_session)TitleOverlay(pixels,pitch,fr.native_video);
#endif
    Xbox_D3D_PrepareFrame();
#ifndef ZAMN_RELEASE_NO_DIAGNOSTICS
    DWORD t2 = GetTickCount();
#endif

    if (audio_ok) {
      zamn_runtime_audio(runtime, audio, ZAMN_AUDIO_SAMPLES_PER_FRAME);
      Xbox_Audio_Submit(audio, ZAMN_AUDIO_SAMPLES_PER_FRAME);
    }
#ifndef ZAMN_RELEASE_NO_DIAGNOSTICS
    DWORD t3 = GetTickCount();
#endif
    Xbox_D3D_Present();
#ifndef ZAMN_RELEASE_NO_DIAGNOSTICS
    DWORD t4 = GetTickCount();

    core_ms += t1 - t0;
    video_ms += t2 - t1;
    audio_ms += t3 - t2;
    present_ms += t4 - t3;
#endif
#ifdef ZAMN_R47_NETPLAY
    if (Xbox_Netplay_Active()) {
      uint64_t stateHash = 0;
#ifdef ZAMN_R48_ROLLBACK
      if (Xbox_Netplay_RollbackEnabled()) {
        if (((frame + 1) % 30) == 0) {
          stateHash = zamn_runtime_state_hash(runtime);
          R48_RecordHash((uint32_t)frame, stateHash);
        }
        // Predicted frames are not hash-authoritative yet. Commit simulation
        // now, then publish only hashes that are older than the rollback window.
        if (!Xbox_Netplay_AfterFrame((uint32_t)frame, 0)) {
          Xbox_Log("ERROR: R49 rollback netplay fault after frame %lu\n", frame);
          Xbox_Netplay_ReportFatal(ZNP_EXIT_AFTERFRAME, (uint32_t)frame);
          Xbox_FlushLog();
          session_return=true; continue;
        }
        const uint32_t win = Xbox_Netplay_RollbackWindow();
        // Age alone does not confirm a rollback frame. Publish only after the
        // real peer input for the checkpoint has arrived and any correction
        // replay has completed. Scan pending checkpoints so a hash deferred on
        // its nominal confirmation frame is submitted later rather than lost.
        R48_SubmitReadyHashes((uint32_t)frame, win);
        R498_CommitAuthoritativeScores();
      } else
#endif
      {
        if (((frame + 1) % 30) == 0) stateHash = zamn_runtime_state_hash(runtime);
        if (!Xbox_Netplay_AfterFrame((uint32_t)frame, stateHash)) {
          Xbox_Log("ERROR: R49 netplay fault after frame %lu\n", frame);
          Xbox_FlushLog();
          session_return=true; continue;
        }
      }
    }
#endif
    ++frame;

    if (frame == 1) {
#ifdef ZAMN_R47_NETPLAY
      Xbox_Log("frame=1 reached under R49 delay/rollback deterministic owner\n");
#elif defined(ZAMN_R46_APU_COMPACT_TRACE)
      Xbox_Log("frame=1 reached under R46 native platform owner\n");
#elif defined(ZAMN_R45_APU_SET_NATIVE_TRANSFER)
      Xbox_Log("frame=1 reached under R45 native platform owner\n");
#elif defined(ZAMN_R44_NATIVE_BURN_ATTRIB)
      Xbox_Log("frame=1 reached under R44 native platform owner\n");
#elif defined(ZAMN_R42_LEVEL_INTRO_WAIT_CUTOVER)
      Xbox_Log("frame=1 reached under R42 native platform owner\n");
#elif defined(ZAMN_R41_TRANSITION_WAIT_CUTOVER)
      Xbox_Log("frame=1 reached under R41 native platform owner\n");
#elif defined(ZAMN_R36_NATIVE_LEAN)
      Xbox_Log("frame=1 reached under R36 native gameplay lean owner\n");
#elif defined(ZAMN_R35_NATIVE_OBJECT_PIPELINE)
      Xbox_Log("frame=1 reached under R35 native scheduler/object owner\n");
#else
      Xbox_Log("frame=1 reached under R34 native scheduler owner\n");
#endif
    }
#ifndef ZAMN_RELEASE_NO_DIAGNOSTICS
    if ((frame % PERF_WINDOW) == 0) {
#ifdef ZAMN_R48_ROLLBACK
      if (Xbox_Netplay_RollbackEnabled()) {
        Xbox_Log("R49SNAP saves=%lu skipped=%lu refresh=%lu replay-saves=%lu bytes=%llu ticks=%llu window=30\n",
          (unsigned long)s_r49_saves, (unsigned long)s_r49_skips,
          (unsigned long)s_r49_refreshes, (unsigned long)s_r49_replay_saves,
          (unsigned long long)s_r49_save_bytes, (unsigned long long)s_r49_save_ticks);
        s_r49_saves = s_r49_skips = s_r49_refreshes = s_r49_replay_saves = 0;
        s_r49_save_bytes = s_r49_save_ticks = 0;
      }
#endif
      DWORD elapsed = t4 - perf_start;
      unsigned long fps_x10 = elapsed ? (PERF_WINDOW * 10000ul) / elapsed : 0;
      Xbox_Log("PERF frames=%lu-%lu elapsed=%lu fps=%lu.%lu input=%lu core=%lu video=%lu audio=%lu present=%lu ms/window\n",
               frame - PERF_WINDOW + 1, frame, (unsigned long)elapsed,
               fps_x10 / 10, fps_x10 % 10,
               (unsigned long)input_ms, (unsigned long)core_ms,
               (unsigned long)video_ms, (unsigned long)audio_ms,
               (unsigned long)present_ms);
#if defined(ZAMN_R75_PAD_ROUTE_TRACE)
      Xbox_Log("R76LT previous-weapon-applied=%lu\n",
               (unsigned long)zamn_runtime_r76_take_reverse_actions());
#endif
#ifdef ZAMN_R47_NETPLAY
      if (Xbox_Netplay_Active()) {
        XboxNetplayStats ns;
        Xbox_Netplay_GetStats(&ns);
        Xbox_Log("ZNET R49 frame=%lu role=%s slot=%u delay=%u mode=%s auto=%u peer-input=%lu peer-consumed=%lu tx=%lu rx=%lu wait-frames=%lu wait-ms=%lu max-wait=%lu resends=%lu midcopy=%lu reskip=%lu pace=%ums pace-total=%lu paceskip=%lu prewait=%lu/%lums late=%lu rb=%u/%lu/%lu/%lu maxrb=%lu rtt=%lums jitter=%lums hash-ok=%lu fault=%u\n",
                 (unsigned long)ns.current_frame, Xbox_Netplay_Role(),
                 (unsigned)ns.local_slot, (unsigned)ns.delay,
                 ns.delay_auto ? "AUTO" : "MANUAL", (unsigned)ns.auto_delay,
                 (unsigned long)ns.latest_peer_frame,
                 (unsigned long)ns.peer_consumed_frame,
                 (unsigned long)ns.tx_packets, (unsigned long)ns.rx_packets,
                 (unsigned long)ns.wait_frames, (unsigned long)ns.wait_ms,
                 (unsigned long)ns.max_wait_ms, (unsigned long)ns.input_resends,
                 (unsigned long)ns.proactive_copies,
                 (unsigned long)ns.resend_suppressed, (unsigned)ns.pace_ms,
                 (unsigned long)ns.pace_ms_total,
                 (unsigned long)ns.pace_budget_skips,
                 (unsigned long)ns.preinput_wait_frames,
                 (unsigned long)ns.preinput_wait_ms,
                 (unsigned long)ns.late_sample_frames,
                 (unsigned)(Xbox_Netplay_RollbackEnabled()?Xbox_Netplay_RollbackWindow():0),
                 (unsigned long)ns.rollback_predictions,
                 (unsigned long)ns.rollback_corrections,
                 (unsigned long)ns.rollback_replayed_frames,
                 (unsigned long)ns.rollback_max_depth,
                 (unsigned long)ns.rtt_ms, (unsigned long)ns.jitter_ms,
                 (unsigned long)ns.hash_matches, (unsigned)ns.fault);
      }
#endif

      ZamnRuntimeWindowStats st;
      zamn_runtime_take_window_stats(runtime, &st);
      Xbox_Log("R34OWNER timed-native-cycles=%llu emu-machine-cycles=%llu native-calls=%llu staged-native256=%lu fallback=%lu\n",
               (unsigned long long)native_cycles,
               (unsigned long long)machine_cycles,
               (unsigned long long)native_calls,
               native256_frames, fallback_frames);
#if defined(ZAMN_R39_BUFFERED_LOG) && defined(ZAMN_R62_ACTOR_CONTROL_NATIVE)
      Xbox_Log("R64HITS R59=%llu R60=%llu R61=%llu R62=%llu R63=%llu R64=%llu\n",
               (unsigned long long)st.r62_family_hits[0],
               (unsigned long long)st.r62_family_hits[1],
               (unsigned long long)st.r62_family_hits[2],
               (unsigned long long)st.r62_family_hits[3],
               (unsigned long long)st.r62_family_hits[4],
               (unsigned long long)st.r62_family_hits[5]);
      for(int i=0;i<st.r62_top_count;i++) {
        Xbox_Log("R64HOT rank=%d name=%s pc=%02X:%04X hits=%llu\n",
                 i+1,st.r62_top[i].name ? st.r62_top[i].name : "unknown",
                 (unsigned)(st.r62_top[i].pc>>16),
                 (unsigned)(st.r62_top[i].pc&65535),
                 (unsigned long long)st.r62_top[i].hits);
      }
#endif
#if defined(ZAMN_R65_HOT_NATIVE_DISPATCH) && defined(ZAMN_R39_BUFFERED_LOG)
      Xbox_Log("R65DISPATCH direct-hits=%llu direct-misses=%llu total-direct=%llu\n",
               (unsigned long long)st.r65_hot_hits,
               (unsigned long long)st.r65_hot_misses,
               (unsigned long long)(st.r65_hot_hits+st.r65_hot_misses));
#endif
      if (st.dispatch_lookups) {
        const unsigned long hit_x10 =
            (unsigned long)((st.dispatch_hits * 1000ull) / st.dispatch_lookups);
        const unsigned long probe_x100 =
            (unsigned long)((st.dispatch_probes * 100ull) / st.dispatch_lookups);
        Xbox_Log("R34DISPATCH lookups=%llu entry-hits=%llu hit=%lu.%lu%% probes=%llu avg=%lu.%02lu\n",
                 (unsigned long long)st.dispatch_lookups,
                 (unsigned long long)st.dispatch_hits,
                 hit_x10 / 10, hit_x10 % 10,
                 (unsigned long long)st.dispatch_probes,
                 probe_x100 / 100, probe_x100 % 100);
      }
#ifdef ZAMN_R34_TRUE_NATIVE_CUTOVER
      Xbox_Log("R34CUT immediate=%llu chain-hops=%llu snes-cycles-elided=%llu page-rejects=%llu idle-slices=%llu idle-cycles=%llu\n",
               (unsigned long long)st.immediate_calls,
               (unsigned long long)st.chain_hops,
               (unsigned long long)st.cycles_elided,
               (unsigned long long)st.page_rejects,
               (unsigned long long)st.idle_slices,
               (unsigned long long)st.idle_cycles);
#endif
#ifdef ZAMN_R36_NATIVE_LEAN
      Xbox_Log("R36LEAN guard-dry-runs=%llu guard-copy-bytes=%llu guard-declines=%llu\n",
               (unsigned long long)st.guard_dry_runs,
               (unsigned long long)st.guard_bytes,
               (unsigned long long)st.guard_declines);
#if defined(ZAMN_R52_FAST_COLLISION_GUARDS)
      Xbox_Log("R52FAST approved=%llu rejected=%llu saved-copy-bytes=%llu\n",
               (unsigned long long)st.r52_fast_approved,
               (unsigned long long)st.r52_fast_rejected,
               (unsigned long long)st.r52_fast_bytes_avoided);
#ifdef ZAMN_R56_SAFE_OAM_CUTOVER
      Xbox_Log("R56OAM readonly-approved=%llu saved-copy-bytes=%llu\n",
               (unsigned long long)st.r56_oam_approved,
               (unsigned long long)(st.r56_oam_approved * 65536ULL));
#ifdef ZAMN_R57_OAM_COLLISION_PROOF
      Xbox_Log("R57OAM fast-rejected=%llu deferred=%llu saved-copy-bytes=%llu\n",
               (unsigned long long)st.r57_oam_rejected,
               (unsigned long long)st.r57_oam_deferred,
               (unsigned long long)(st.r57_oam_rejected * 65536ULL));
#ifdef ZAMN_R58_COLLISION_THREAD_FASTPATH
      Xbox_Log("R58OAM newly-approved=%llu saved-copy-bytes=%llu\n",
               (unsigned long long)st.r58_oam_new_approved,
               (unsigned long long)(st.r58_oam_new_approved * 65536ULL));
#if defined(ZAMN_R66_TOTAL_HANDLER_FAST_GUARDS)
      Xbox_Log("R66TOTAL newly-approved=%llu saved-copy-bytes=%llu\n",
               (unsigned long long)st.r66_total_approved,
               (unsigned long long)(st.r66_total_approved * 131072ULL));
#if defined(ZAMN_R68_INPUT_READONLY_GUARDS) && defined(ZAMN_R39_BUFFERED_LOG)
      Xbox_Log("R68INPUT oam-approved=%llu thread-approved=%llu saved-copy-bytes=%llu\n",
               (unsigned long long)st.r68_oam_approvals,
               (unsigned long long)st.r68_thread_approvals,
               (unsigned long long)(st.r68_oam_approvals * 65536ULL +
                                    st.r68_thread_approvals * 131072ULL));
#endif
#if defined(ZAMN_R70_HOT_THREAD_9A6D) && defined(ZAMN_R39_BUFFERED_LOG)
      Xbox_Log("R70THREAD hot-9A6D-approved=%llu saved-copy-bytes=%llu\n",
               (unsigned long long)st.r70_thread_9a6d_approved,
               (unsigned long long)(st.r70_thread_9a6d_approved * 131072ULL));
#endif
#if defined(ZAMN_R74_COLLISION_OAM_INPUT_PROOF) && defined(ZAMN_R39_BUFFERED_LOG)
      Xbox_Log("R74PAIR notify-approved=%llu oam-approved=%llu saved-copy-bytes=%llu\n",
               (unsigned long long)st.r74_notify_approvals,
               (unsigned long long)st.r74_oam_approvals,
               (unsigned long long)(st.r74_notify_approvals * 131072ULL +
                                    st.r74_oam_approvals * 65536ULL));
#endif
#if defined(ZAMN_R74_COLLISION_OAM_INPUT_PROOF) && defined(ZAMN_R69_PORTING_PROFILE) && defined(ZAMN_R39_BUFFERED_LOG) && !defined(ZAMN_RELEASE_NO_DIAGNOSTICS)
      for(unsigned k=0;k<ZAMN_R74_PAIR_TOP;k++) {
        const ZamnR74PairStat* p=&st.r74_pair_hot[k];
        if (!p->calls)continue;
        Xbox_Log("R74PAIRHOT rank=%u first=%06X arg=%04X second=%06X arg=%04X calls=%u declines=%u\n",
                 k+1u,(unsigned)p->first,(unsigned)p->first_arg,
                 (unsigned)p->second,(unsigned)p->second_arg,
                 (unsigned)p->calls,(unsigned)p->declined);
      }
      if (st.r74_pair_untracked)
        Xbox_Log("R74PAIRHOT overflow=%llu\n",(unsigned long long)st.r74_pair_untracked);
#endif
#if defined(ZAMN_R67_GUARD_REASON_PROFILE) && defined(ZAMN_R39_BUFFERED_LOG) && !defined(ZAMN_RELEASE_NO_DIAGNOSTICS)
      // Codes: 0=defer, 1/3/4/5=approve, 2=reject. Separate true
      // unported rejects from deferrals that still incur a scratch copy.
      for (int k=0; k<3; ++k) {
        const char* n = k==0 ? "thread_call_handler" :
                        k==1 ? "actor_collide_notify" : "other";
        const unsigned long long a =
            (unsigned long long)(st.r67_fast_reasons[k][1] +
            st.r67_fast_reasons[k][3] + st.r67_fast_reasons[k][4] +
            st.r67_fast_reasons[k][5]);
        Xbox_Log("R67GUARD name=%s approve=%llu reject-unported=%llu defer=%llu defer-bad-db=%llu sandbox-pass=%llu sandbox-decline=%llu\n",
                 n, a,
                 (unsigned long long)st.r67_fast_reasons[k][2],
                 (unsigned long long)st.r67_fast_reasons[k][0],
                 (unsigned long long)st.r67_defer_bad_db[k],
                 (unsigned long long)st.r67_sandbox_results[k][1],
                 (unsigned long long)st.r67_sandbox_results[k][0]);
      }
#endif
#endif
      Xbox_Log("R58NOTIFY newly-approved=%llu saved-copy-bytes=%llu\n",
               (unsigned long long)st.r58_notify_new_approved,
               (unsigned long long)(st.r58_notify_new_approved * 131072ULL));
#endif
#endif
#endif
#endif
#if defined(ZAMN_R70_PAUSE_TRACE) && defined(ZAMN_R39_BUFFERED_LOG) && !defined(ZAMN_RELEASE_NO_DIAGNOSTICS)
      Xbox_Log("R70PAUSE release1=%llu press=%llu release2=%llu start-down=%llu/%llu/%llu\n",
               (unsigned long long)st.r70_pause_polls[0],
               (unsigned long long)st.r70_pause_polls[1],
               (unsigned long long)st.r70_pause_polls[2],
               (unsigned long long)st.r70_pause_start_down[0],
               (unsigned long long)st.r70_pause_start_down[1],
               (unsigned long long)st.r70_pause_start_down[2]);
#endif
#ifdef ZAMN_R39_BUFFERED_LOG
      for (int gi=0; gi<ZAMN_GUARD_PROFILE_TOP; ++gi) {
        const ZamnGuardProfileHot& g=st.guard_hot[gi];
        if (!g.calls) break;
        Xbox_Log("R51GUARD rank=%d name=%s pc=%02X:%04X calls=%llu bytes=%llu declined=%llu\n",
                 gi+1, g.name ? g.name : "unknown",
                 (unsigned)((g.pc>>16)&255), (unsigned)(g.pc&65535),
                 (unsigned long long)g.calls, (unsigned long long)g.bytes,
                 (unsigned long long)g.declines);
      }
#endif
#ifdef ZAMN_R37_READONLY_GUARDS
#if defined(ZAMN_R69_PORTING_PROFILE) && defined(ZAMN_R39_BUFFERED_LOG) && !defined(ZAMN_RELEASE_NO_DIAGNOSTICS)
      Xbox_Log("R69AUDIO apu-runs=%llu spc-opcodes=%llu apu-cycles=%llu samples=%llu dsp-writes=%llu cpu-to-spc=%llu/%llu/%llu/%llu\n",
               (unsigned long long)st.r69_apu.run_calls,
               (unsigned long long)st.r69_apu.executed_opcodes,
               (unsigned long long)st.r69_apu.executed_cycles,
               (unsigned long long)st.r69_apu.total_samples,
               (unsigned long long)st.r69_apu.dsp_writes,
               (unsigned long long)st.r69_apu.port_writes[0],
               (unsigned long long)st.r69_apu.port_writes[1],
               (unsigned long long)st.r69_apu.port_writes[2],
               (unsigned long long)st.r69_apu.port_writes[3]);
      for (int k=0; k<ZAMN_R69_SPC_TOP && st.r69_apu.page_samples[k]; ++k)
        Xbox_Log("R69SPC rank=%d page=%04X-%04X pc-samples=%lu opcode=%02X opcode-samples=%lu\n",
          k+1, (unsigned)st.r69_apu.pc_page[k],
          (unsigned)(st.r69_apu.pc_page[k]+15u),
          (unsigned long)st.r69_apu.page_samples[k],
          (unsigned)st.r69_apu.opcode[k],
          (unsigned long)st.r69_apu.opcode_samples[k]);
      for (int k=0; k<ZAMN_R69_AUDIO_CMD_TOP && st.r69_apu.command_writes[k]; ++k)
        Xbox_Log("R69SOUND rank=%d port2142-value=%02X writes=%lu (candidate sound command, not confirmed SFX ID)\n",
          k+1, (unsigned)st.r69_apu.command[k],
          (unsigned long)st.r69_apu.command_writes[k]);
      for (int k=0; k<ZAMN_R69_GUARD_TOP && st.r69_guard_hot[k].calls; ++k) {
        const ZamnR69GuardHot& p = st.r69_guard_hot[k];
        Xbox_Log("R69GUARD rank=%d guard=%02X:%04X handler=%02X:%04X copies=%llu bytes=%llu declines=%llu sample-arg=%04X sample-db=%02X%s\n",
          k+1, (unsigned)(p.guard_pc>>16), (unsigned)(p.guard_pc&0xffffu),
          (unsigned)(p.handler_pc>>16), (unsigned)(p.handler_pc&0xffffu),
          (unsigned long long)p.calls, (unsigned long long)p.bytes,
          (unsigned long long)p.declines, (unsigned)p.example_arg,
          (unsigned)(p.example_db&0xffu),
          p.handler_pc ? "" : " (no unique handler attribution)");
      }
      if (st.r69_guard_untracked)
        Xbox_Log("R69GUARD untracked-unique-cases=%llu (profiler table full)\n",
          (unsigned long long)st.r69_guard_untracked);
#endif
      Xbox_Log("R37GUARD readonly=%llu copy-bytes-avoided=%llu readonly-declines=%llu\n",
               (unsigned long long)st.guard_readonly_checks,
               (unsigned long long)st.guard_bytes_avoided,
               (unsigned long long)st.guard_readonly_declines);
#endif
#ifdef ZAMN_R41_TRANSITION_WAIT_CUTOVER
      Xbox_Log("R41WAIT slices=%llu cycles=%llu 80:9FAA=%llu 80:923A=%llu 80:924C=%llu 80:9F5C=%llu\n",
               (unsigned long long)st.transition_wait_slices,
               (unsigned long long)st.transition_wait_cycles,
               (unsigned long long)st.transition_wait_sites[0],
               (unsigned long long)st.transition_wait_sites[1],
               (unsigned long long)st.transition_wait_sites[2],
               (unsigned long long)st.transition_wait_sites[3]);
#endif
#ifdef ZAMN_R42_LEVEL_INTRO_WAIT_CUTOVER
      Xbox_Log("R42INTRO slices=%llu cycles=%llu 82:AC65=%llu 82:AC92=%llu pattern-rejects=%llu\n",
               (unsigned long long)st.intro_wait_slices,
               (unsigned long long)st.intro_wait_cycles,
               (unsigned long long)st.intro_wait_sites[0],
               (unsigned long long)st.intro_wait_sites[1],
               (unsigned long long)st.intro_wait_pattern_rejects);
#endif
#ifdef ZAMN_R43_NATIVE_WAIT_CUTOVER
      Xbox_Log("R43WAIT slices=%llu cycles=%llu 80:81B5=%llu 80:933A=%llu rejects-81B5=%llu rejects-933A=%llu\n",
               (unsigned long long)st.r43_wait_slices,
               (unsigned long long)st.r43_wait_cycles,
               (unsigned long long)st.r43_wait_sites[0],
               (unsigned long long)st.r43_wait_sites[1],
               (unsigned long long)st.r43_wait_rejects[0],
               (unsigned long long)st.r43_wait_rejects[1]);
#endif
#ifdef ZAMN_R45_APU_SET_NATIVE_TRANSFER
      Xbox_Log("R45APU calls=%llu run-events-elided=%llu snes-cycles-elided=%llu\n",
               (unsigned long long)st.r45_apu_set_calls,
               (unsigned long long)st.r45_apu_set_run_events,
               (unsigned long long)st.r45_apu_set_cycles_elided);
#endif
#ifdef ZAMN_R46_APU_COMPACT_TRACE
      Xbox_Log("R46APU calls=%llu run-steps-elided=%llu run-reps-elided=%llu snes-cycles-elided=%llu stack-coalesced=%llu trace-steps=%llu\n",
               (unsigned long long)st.r46_apu_trace_calls,
               (unsigned long long)st.r46_apu_run_steps_elided,
               (unsigned long long)st.r46_apu_run_reps_elided,
               (unsigned long long)st.r46_apu_cycles_elided,
               (unsigned long long)st.r46_apu_stack_coalesced,
               (unsigned long long)st.r46_apu_trace_steps_emitted);
#endif
#ifdef ZAMN_R46_STRICT_NATIVE_SHARE
      if (st.r46_work_equiv) {
        const unsigned long native_x10 = (unsigned long)(
            (st.r46_native_equiv * 1000ull) / st.r46_work_equiv);
        const unsigned long strict_x10 = (unsigned long)(
            (st.r46_native_cpu_equiv * 1000ull) / st.r46_work_equiv);
        Xbox_Log("R46SHARE native-eq=%lu.%lu%% strict-cpu=%lu.%lu%% native=%llu strict=%llu work=%llu\n",
                 native_x10 / 10, native_x10 % 10,
                 strict_x10 / 10, strict_x10 % 10,
                 (unsigned long long)st.r46_native_equiv,
                 (unsigned long long)st.r46_native_cpu_equiv,
                 (unsigned long long)st.r46_work_equiv);
      }
#endif
#ifdef ZAMN_R44_NATIVE_BURN_ATTRIB
      if (st.r44_burn_top_count > 0) {
        Xbox_Log("R44BURN top1=%s(%s) cycles=%llu calls=%llu",
                 st.r44_burn_top[0].name ? st.r44_burn_top[0].name : "?",
                 st.r44_burn_top[0].symbol ? st.r44_burn_top[0].symbol : "?",
                 (unsigned long long)st.r44_burn_top[0].cycles,
                 (unsigned long long)st.r44_burn_top[0].calls);
        if (st.r44_burn_top_count > 1)
          Xbox_Log(" top2=%s(%s) cycles=%llu calls=%llu",
                   st.r44_burn_top[1].name ? st.r44_burn_top[1].name : "?",
                   st.r44_burn_top[1].symbol ? st.r44_burn_top[1].symbol : "?",
                   (unsigned long long)st.r44_burn_top[1].cycles,
                   (unsigned long long)st.r44_burn_top[1].calls);
        if (st.r44_burn_top_count > 2)
          Xbox_Log(" top3=%s(%s) cycles=%llu calls=%llu",
                   st.r44_burn_top[2].name ? st.r44_burn_top[2].name : "?",
                   st.r44_burn_top[2].symbol ? st.r44_burn_top[2].symbol : "?",
                   (unsigned long long)st.r44_burn_top[2].cycles,
                   (unsigned long long)st.r44_burn_top[2].calls);
        Xbox_Log("\n");
      } else {
        Xbox_Log("R44BURN none\n");
      }
#endif
#ifdef ZAMN_R41_NATIVE_RENDER_OWNER
      Xbox_Log("R41VIDEO owned-lines=%llu fallback-lines=%llu\n",
               (unsigned long long)st.native_renderer_lines,
               (unsigned long long)st.native_renderer_fallbacks);
#endif
#ifdef ZAMN_R291_HOT_RESIDUE
      {
        char hot[512];
        int at = snprintf(hot, sizeof(hot),
                           "R36RESIDUAL samples=%lu dropped=%lu",
                           (unsigned long)st.residual_samples,
                           (unsigned long)st.residual_dropped);
        for (int i = 0; i < st.residual_count && at > 0 && at < (int)sizeof(hot) - 24; ++i) {
          const uint32_t pc = st.residual_pc[i];
          at += snprintf(hot + at, sizeof(hot) - at, " %02lX:%04lX=%lu",
                          (unsigned long)((pc >> 16) & 0xffu),
                          (unsigned long)(pc & 0xffffu),
                          (unsigned long)st.residual_hits[i]);
        }
        Xbox_Log("%s\n", hot);
      }
#endif
#endif
      Xbox_Log("R271PATH mode1=%llu submath-lines=%llu\n",
               (unsigned long long)st.mode1_fast_lines,
               (unsigned long long)st.submath_lines);
#ifdef ZAMN_R35_NATIVE_OBJECT_PIPELINE
      Xbox_Log("R35OBJ cache-builds=%llu cached-lines=%llu mmx-slivers=%llu scalar-slivers=%llu\n",
               (unsigned long long)st.sprite_cache_builds,
               (unsigned long long)st.sprite_cached_lines,
               (unsigned long long)st.sprite_mmx_slivers,
               (unsigned long long)st.sprite_scalar_slivers);
#endif
#ifndef ZAMN_STOCK_CORE
      Xbox_Log("R27NATIVE ticks=%llu fused=%llu yield=%llu unknown=%llu h8600=%llu h8656=%llu h86b3=%llu random=%llu\n",
               (unsigned long long)st.zombie_calls,
               (unsigned long long)st.zombie_fused,
               (unsigned long long)st.zombie_yield,
               (unsigned long long)st.zombie_unknown,
               (unsigned long long)st.zombie_h8600,
               (unsigned long long)st.zombie_h8656,
               (unsigned long long)st.zombie_h86b3,
               (unsigned long long)st.zombie_random);
      Xbox_Log("R29ACTOR chase=%lu native=%lu leap-fallback=%lu actor=%lu native=%lu fallback=%lu\n",
               (unsigned long)st.actor_counts[0],
               (unsigned long)st.actor_counts[1],
               (unsigned long)st.actor_counts[2],
               (unsigned long)st.actor_counts[3],
               (unsigned long)st.actor_counts[4],
               (unsigned long)st.actor_counts[5]);
      {
        const uint64_t saved = st.burn_legacy12 > st.burn_calls
                                   ? st.burn_legacy12 - st.burn_calls : 0;
        Xbox_Log("R28BURN calls=%llu legacy12=%llu saved=%llu cycles=%llu\n",
                 (unsigned long long)st.burn_calls,
                 (unsigned long long)st.burn_legacy12,
                 (unsigned long long)saved,
                 (unsigned long long)st.burn_cycles);
      }
#endif

#ifdef ZAMN_R39_BUFFERED_LOG
      // Diagnostic builds must leave useful gameplay data on disk even when
      // a test ends with a reset or an FTP copy rather than a normal exit.
      // This block is absent from no-diagnostics release builds. At 60 FPS,
      // flush once per five seconds, outside the next PERF timing window.
      if ((frame % 300ul) == 0) Xbox_FlushLog();
      // The old code restarted from t4, then spent time synchronously writing
      // the diagnostics above.  That logger stall was charged to the *next*
      // 30-frame window and produced false/real periodic FPS dips.
      perf_start = GetTickCount();
#else
      perf_start = t4;
#endif
      input_ms = core_ms = video_ms = audio_ms = present_ms = 0;
      core_ticks = 0;
      machine_cycles = native_cycles = native_calls = 0;
      native256_frames = fallback_frames = 0;
    }
#endif
  }

  Xbox_Log("exit requested\n");
  Xbox_FlushLog();
#ifdef ZAMN_R47_NETPLAY
#ifdef ZAMN_R48_ROLLBACK
  R48_RollbackFree();
#endif
  Xbox_Netplay_Shutdown();
#endif
  Xbox_Input_Shutdown();
  Xbox_Audio_Shutdown();
  Xbox_D3D_Shutdown();
  zamn_runtime_destroy(runtime);
  if (remasteredBank) { free(remasteredBank); remasteredBank = NULL; }
#if !defined(ZAMN_RELEASE_NO_DIAGNOSTICS) || defined(ZAMN_R499_SERVICE_LOG)
  if (s_log) { fclose(s_log); s_log = NULL; }
#endif
  return 0;
}
