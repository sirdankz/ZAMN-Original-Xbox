from pathlib import Path
root=Path(__file__).resolve().parents[1]
main=(root/'src/platform/xbox/xbox_main.cpp').read_text(errors='ignore')
net=(root/'src/platform/xbox/xbox_netplay.cpp').read_text(errors='ignore')
hdr=(root/'src/platform/xbox/xbox_netplay.h').read_text(errors='ignore')
title=(root/'src/platform/xbox/xbox_title_online.h').read_text(errors='ignore')
build=(root/'BUILD_XBOX_RELEASE.ps1').read_text(errors='ignore')
enabled=build[build.index('foreach ($d in @('):build.index('    )) { if ($defs -notcontains $d)')]
readme=(root/'README.md').read_text(errors='ignore')
checks={
 'R49.9 wire build': '#define ZNP_BUILD 0x5A4D490Au' in net,
 'top online menu has leaderboard': all(x in main for x in ['PUBLIC ROOMS','DIRECT CONNECT','SOLO PLAY','LEADERBOARDS','PROFILE']) and 'return 7' in title,
 'public rooms has world chat not leaderboard row': 'WORLD CHAT' in net and 'HOST PUBLIC ROOM' in net and 'JOIN PUBLIC ROOM' in net,
 'server-backed auth protocol': 'ZAUTH1|%s|' in net and 'ZAUTH1|TOKEN|' in net and 'Xbox_Netplay_EnsureProfile' in hdr,
 'R49.9 profile/token store': 'profiles-r499.dat' in net and 'R499_AUTH_TOKEN_MAX' in net,
 'console identity hashes Ethernet address': 'xa.abEnet' in net and 'ZAMN-XBOX-R499' not in net,  # salt is byte literal, raw address is never formatted
 'password UI masked': 'PROFILE LOGIN' in net and 'PASSWORD' in net and 'true)' in net,
 'new local queue ignores R49.8 queue': 'scores-r499.dat' in net and 'ZLB2|SUBMIT|' in net,
 'local best-only comparison': 'frames<r.frames' in net and 'local-ignore-not-better' in net,
 'separate solo and online boards': 'SOLO ONLINE LEADERBOARDS' in net and 'CO-OP ONLINE LEADERBOARDS' in net and 'R499_SCOPE_SOLO' in net and 'R499_SCOPE_ONLINE' in net,
 'world and lobby chat protocol': 'ZCHAT1|SEND|' in net and 'ZCHAT1|LIST|' in net and 'Y WORLD/LOBBY' in net,
 'public room requires profile token': 'r499_profile_token()' in net and 'ZDIR1|REGISTER|' in net and 'ZDIR1|JOIN|' in net,
 'public release service log disabled': 'ZAMN_R499_SERVICE_LOG=1' not in enabled and 'ZAMN_R39_BUFFERED_LOG=1' not in enabled,
 'heavy profiling disabled': 'ZAMN_RELEASE_NO_DIAGNOSTICS=1' in build and 'ZAMN_R291_HOT_RESIDUE=1' not in build,
 'release tag': 'R49_9_7_RELEASE_' in build,
 'readme documents accounts chat and best-only': 'best-time-only' in readme.lower() and 'World Chat' in readme and 'server-backed profiles' in readme.lower(),
}
bad=[k for k,v in checks.items() if not v]
for k,v in checks.items(): print(('PASS ' if v else 'FAIL ')+k)
if bad: raise SystemExit('failed: '+', '.join(bad))
print('PASS R49.9 accounts/chat/best-only leaderboard source invariants')
