from pathlib import Path
r=Path(__file__).resolve().parents[1]
main=(r/'src/platform/xbox/xbox_main.cpp').read_text()
audio=(r/'src/platform/xbox/xbox_audio.cpp').read_text()
hdr=(r/'src/platform/xbox/xbox_platform.h').read_text()
net=(r/'src/platform/xbox/xbox_netplay.cpp').read_text()
build=(r/'BUILD_XBOX_RELEASE.ps1').read_text()
checks={
 'cumulative R49.9 wire build': '#define ZNP_BUILD 0x5A4D490Au' in net,
 'R49.5 cache ABI retained': '#define R495_TITLE_CACHE_BUILD 0x5A4D4905u' in main,
 'R49.5 cache path retained': 'zamn-r49_5-title-cache.bin' in main,
 'mute API public': 'void Xbox_Audio_SetMuted(bool muted);' in hdr,
 'audio thread honors mute': 'if (s_muted) { Sleep(2); continue; }' in audio,
 'submit drops muted PCM': 'frames != BLOCK_FRAMES || s_muted' in audio,
 'hardware volume mutes': 'SetVolume(DSBVOLUME_MIN)' in audio,
 'hardware loop cleared': 's_buf->Lock(0, DS_BYTES' in audio and 'ZeroMemory(p1, n1)' in audio,
 'queued PCM discarded': audio.count('InterlockedExchange(&s_rp, s_wp)') >= 2,
 'prepare mutes immediately': main.find('Xbox_Audio_SetMuted(true);') > main.find('static int TitlePrepareMatch(bool begin)'),
 'resume waits for netplay open': main.find('Xbox_Audio_SetMuted(false);', main.find('const bool connected=Xbox_Netplay_Open(action);')) > main.find('const bool connected=Xbox_Netplay_Open(action);'),
 'resume status logged': 'R49.6 start audio resumed connected=' in main,
 'instant start retained': 'R49.5 instant canonical start' in main,
 'build script checks audio source': "'src\\platform\\xbox\\xbox_audio.cpp'" in build,
 'R49.7 release output tag': 'R49_9_7_RELEASE_' in build,
}
for k,v in checks.items(): print(('PASS' if v else 'FAIL'),k)
assert all(checks.values())
print('PASS R49.6 start-audio-mute source invariants')
