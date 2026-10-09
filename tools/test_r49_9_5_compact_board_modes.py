from pathlib import Path
root=Path(__file__).resolve().parents[1]
net=(root/'src/platform/xbox/xbox_netplay.cpp').read_text(errors='ignore')
main=(root/'src/platform/xbox/xbox_main.cpp').read_text(errors='ignore')
build=(root/'BUILD_XBOX_RELEASE.ps1').read_text(errors='ignore')
readme=(root/'README.md').read_text(errors='ignore')
checks={
 'R49.9.5 boot marker':'R49.9.5 COMPACT-BOARDS/MODE-LABELS' in main,
 'wire build unchanged':'#define ZNP_BUILD 0x5A4D490Au' in net,
 'compact date helper':"src[4]=='-'" in net and 'strcpy(out,"--/--/--")' in net,
 'date same line as time':'%s  %s' in net and 'r4995_compact_date(rows[i].date,date)' in net,
 'no redundant set marker':'SET %s' not in net,
 'no redundant player-count row':'%uP   SET' not in net,
 'solo category preserved':'SOLO ONLINE LEADERBOARDS' in net,
 'coop category preserved':'CO-OP ONLINE LEADERBOARDS' in net,
 'normal save explanation':'NORMAL / SAVABLE (+1 LIFE EVERY 5 LEVELS)' in net,
 'normal nosave explanation':'NORMAL / NO SAVE (+1 LIFE EVERY 5 LEVELS)' in net,
 'hardcore save explanation':'HARDCORE / SAVABLE (ORIGINAL GAMEPLAY)' in net,
 'hardcore nosave explanation':'HARDCORE / NO SAVE (ORIGINAL GAMEPLAY)' in net,
 'long labels limited to picker':'r4995_ruleset_menu_name' in net and 'r498_ruleset_name(ruleset)' in net,
 'release tag':'R49_9_7_RELEASE_' in build,
 'README documents compact board':'MM:SS.mmm' in readme and 'server record date' in readme.lower(),
}
bad=[]
for k,v in checks.items():
 print(('PASS ' if v else 'FAIL ')+k)
 if not v: bad.append(k)
if bad: raise SystemExit('failed: '+', '.join(bad))
print('PASS R49.9.5 compact leaderboard/mode-label source invariants')
