from pathlib import Path
root=Path(__file__).resolve().parents[1]
net=(root/'src/platform/xbox/xbox_netplay.cpp').read_text(errors='ignore')
main=(root/'src/platform/xbox/xbox_main.cpp').read_text(errors='ignore')
title=(root/'src/platform/xbox/xbox_title_online.h').read_text(errors='ignore')
hdr=(root/'src/platform/xbox/xbox_netplay.h').read_text(errors='ignore')
build=(root/'BUILD_XBOX_RELEASE.ps1').read_text(errors='ignore')
readme=(root/'README.md').read_text(errors='ignore')
checks={
 'R49.9.4 boot marker':'R49.9.4 UI/PROFILE/RECORD-DATES' in main,
 'wire build unchanged':'#define ZNP_BUILD 0x5A4D490Au' in net,
 'online menu has five choices':all(x in main for x in ['PUBLIC ROOMS','DIRECT CONNECT','SOLO PLAY','LEADERBOARDS','PROFILE']) and 'HOST DIRECT","JOIN DIRECT' not in main,
 'two-row title coordinates':'const int ys[]={194,194,210,210,210};' in main and 'const int ys[]={194,194,210};' in main,
 'direct submenu exported':'Xbox_Netplay_DirectMenu' in hdr and 'DIRECT CONNECT' in net and 'HOST DIRECT' in net and 'JOIN DIRECT' in net,
 'fresh profiles empty':'s_profiles.count=0;s_profiles.active=0' in net and 'count=1;s_profiles.active=0;strcpy' not in net,
 'recover asks username and password':'SIGN IN / RECOVER","USERNAME"' in net and 'SIGN IN / RECOVER","PASSWORD"' in net,
 'keyboard latches held input':'Xbox_Input_Poll();uint16_t prev=Xbox_Input_GetJoypad(0);menu_latch_current();' in net,
 'dated leaderboard row':'char date[11]' in net,
 'dated packed request':'LISTD' in net and 'PACKD' in net,
 'leaderboard fixed layout':'SOLO ONLINE LEADERBOARDS' in net and 'CO-OP ONLINE LEADERBOARDS' in net and 'char date[11]' in net,
 'release tag':'R49_9_7_RELEASE_' in build,
 'README documents record dates':'server record date' in readme.lower(),
}
bad=[]
for k,v in checks.items():
 print(('PASS ' if v else 'FAIL ')+k)
 if not v: bad.append(k)
if bad: raise SystemExit('failed: '+', '.join(bad))
print('PASS R49.9.4 UI/account/date source invariants')
