from pathlib import Path
root=Path(__file__).resolve().parents[1]
main=(root/'src/platform/xbox/xbox_main.cpp').read_text(errors='ignore')
net=(root/'src/platform/xbox/xbox_netplay.cpp').read_text(errors='ignore')
audio=(root/'src/platform/xbox/xbox_audio.cpp').read_text(errors='ignore')
build=(root/'BUILD_XBOX_RELEASE.ps1').read_text(errors='ignore')
checks={
 'loading overlay helper':'R4992_ShowLoadingOverlay' in main and '"LOADING..."' in main,
 'online exit shows overlay before mute':main.index('R4992_ShowLoadingOverlay();', main.index('if(session_return)')) < main.index('Xbox_Audio_SetMuted(true);', main.index('if(session_return)')),
 'solo exit shows overlay':'solo return-to-title loading overlay + audio hard-stop' in main,
 'nested mute depth':'s_mute_depth' in audio and 'InterlockedIncrement(&s_mute_depth)' in audio and 'InterlockedDecrement(&s_mute_depth)' in audio,
 'hard stop on mute':'s_buf->Stop();' in audio,
 'restart on outer unmute':'s_buf->Play(0, 0, DSBPLAY_LOOPING);' in audio,
 'R49.9.2 retry storm removed by successor':'partial-retry' not in net and 'r499_board_async_tick' in net,
 'solo online label':'SOLO ONLINE LEADERBOARDS' in net,
 'coop online label':'CO-OP ONLINE LEADERBOARDS' in net,
 'direct discovery loading mute':'PUBLIC IP DISCOVERY' in net and '{ R499BlockingAudioMute audio_mute; discover_public_ip(); }' in net,
 'release tag':'R49_9_7_RELEASE_' in build,
}
for k,v in checks.items():
    print(('PASS ' if v else 'FAIL ')+k)
if not all(checks.values()): raise SystemExit(1)
print('PASS R49.9.2 frontend polish source invariants')
