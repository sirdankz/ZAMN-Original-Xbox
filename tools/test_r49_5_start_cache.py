from pathlib import Path
r=Path(__file__).resolve().parents[1]
main=(r/'src/platform/xbox/xbox_main.cpp').read_text()
runtime=(r/'src/native/runtime.c').read_text()
hdr=(r/'src/native/runtime.h').read_text()
net=(r/'src/platform/xbox/xbox_netplay.cpp').read_text()
checks={
 'cumulative R49.9 build id': '#define ZNP_BUILD 0x5A4D490Au' in net,
 'R49.5 cache ABI': '#define R495_TITLE_CACHE_BUILD 0x5A4D4905u' in main,
 'persistent cache path': 'zamn-r49_5-title-cache.bin' in main,
 'cache ROM guard': 'h.rom_hash!=s_title_rom_hash' in main,
 'cache audio guard': 'h.audio_key!=TitleAudioKey()' in main,
 'restore hash verification': 'title_hash!=s_title_cache_hash' in main,
 'instant start log': 'R49.5 instant canonical start' in main,
 'one-time fallback': 'fallback prepare begun' in main and 'zamn_runtime_title_reset_step(s_title_runtime,false)' in main,
 'cache-before-start': main.find('TitleCacheWriteCanonical();') < main.find('zamn_runtime_title_start(s_title_runtime)', main.find('TitleCacheWriteCanonical();')),
 'instant return-to-title': 'if(!TitleRestoreCanonical())break;' in main,
 'public title-start API': 'bool zamn_runtime_title_start(ZamnNativeRuntime* rt);' in hdr,
 'title-start implementation': 'bool zamn_runtime_title_start(ZamnNativeRuntime* rt)' in runtime,
 'persistent audio config safety': 'current_bank = rt->audio.bank' in runtime and 'rt->audio.bank = current_bank' in runtime and 'current_samples[256]' in runtime and 'rt->audio.mode = current_mode' in runtime,
}
for k,v in checks.items(): print(('PASS' if v else 'FAIL'),k)
assert all(checks.values())
print('PASS R49.5 instant-start source invariants')
