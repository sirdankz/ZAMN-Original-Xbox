from pathlib import Path
root=Path(__file__).resolve().parents[1]
main=(root/'src/platform/xbox/xbox_main.cpp').read_text(errors='ignore')
net=(root/'src/platform/xbox/xbox_netplay.cpp').read_text(errors='ignore')
build=(root/'BUILD_XBOX_RELEASE.ps1').read_text(errors='ignore')
readme=(root/'README.md').read_text(errors='ignore')
checks={
 'wire-compatible R49.9 build id': '#define ZNP_BUILD 0x5A4D490Au' in net,
 'session auth cache state': 's_r499_session_auth' in net and 's_r499_session_auth_name' in net,
 'ensure profile cache hit avoids reauth': 'AUTH session-cache' in net and 's_r499_session_auth&&!strcmp(s_r499_session_auth_name,e->name)' in net,
 'successful auth seeds session cache': 's_r499_session_auth=true' in net and 's_r499_session_auth_name,clean' in net,
 'blocking service audio mute helper': 'class R499BlockingAudioMute' in net and 'Xbox_Audio_SetMuted(true)' in net and 'Xbox_Audio_SetMuted(false)' in net,
 'auth wait audio protected': 'R499BlockingAudioMute audio_mute;' in net[net.index('static bool r499_profile_auth_dialog'):net.index('static bool r499_same_score_key')],
 'leaderboard no longer blocks audio/menu': 'r499_board_async_tick' in net and 'static int r498_fetch_board' not in net,
 'chat waits audio protected': net[net.index('static int r499_chat_fetch'):net.index('static void r499_chat_menu', net.index('static int r499_chat_fetch'))].count('R499BlockingAudioMute audio_mute;') >= 2,
 'public room list audio protected': 'static int public_fetch_rooms' in net and 'static int public_fetch_rooms(PublicRoomEntry rooms[ZDIR_MAX_ROOMS]){\n  R499BlockingAudioMute audio_mute;' in net,
 'online exit mutes before teardown': 'return-to-title loading overlay + audio hard-stop' in main and main.index('return-to-title loading overlay + audio hard-stop') < main.index('Xbox_Netplay_Shutdown();', main.index('return-to-title loading overlay + audio hard-stop')), 
 'online exit unmutes after canonical restore': 'R49.9.2 return-to-title ready' in main and 'TitleRestoreCanonical()){Xbox_Audio_SetMuted(false);break;}' in main,
 'solo exit audio protected': 'R49.9.2 solo return-to-title loading overlay + audio hard-stop' in main and 'R49.9.2 solo return-to-title ready' in main,
 'R49.9.1 release tag': 'R49_9_7_RELEASE_' in build,
 'README documents cached title return': 'Fast cached title return' in readme,
}
bad=[]
for k,v in checks.items():
    print(('PASS ' if v else 'FAIL ')+k)
    if not v: bad.append(k)
if bad: raise SystemExit('failed: '+', '.join(bad))
print('PASS R49.9.1 menu/audio/session-auth source invariants')
