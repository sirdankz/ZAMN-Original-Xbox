from pathlib import Path
root=Path(__file__).resolve().parents[1]
main=(root/'src/platform/xbox/xbox_main.cpp').read_text(errors='ignore')
net=(root/'src/platform/xbox/xbox_netplay.cpp').read_text(errors='ignore')
run=(root/'src/native/runtime.c').read_text(errors='ignore')
hdr=(root/'src/native/runtime.h').read_text(errors='ignore')
build=(root/'BUILD_XBOX_RELEASE.ps1').read_text(errors='ignore')
readme=(root/'README.md').read_text(errors='ignore')
checks={
 'cumulative R49.9 wire build': '#define ZNP_BUILD 0x5A4D490Au' in net,
 'exit-door runtime probe': 'zamn_runtime_exit_door_last' in run and '0x1fbcu' in run and 'zamn_runtime_exit_door_last' in hdr,
 'clock tracks exit players': 'exit_mask' in main and 'R498_RequiredExitMask' in main,
 'solo door commits immediately': 'door==5u' in main and 'score_recorded=true' in main,
 'level change fallback retained': 'need_fallback' in main,
 'normal reward remains boundary-based': 'completed%5u' in main and 'zamn_runtime_normal_checkpoint_reward' in main,
 'millisecond time UI': '*1000ull' in net and '%02lu:%02lu.%03lu' in net,
 'pending leaderboard fallback': 'SYNCING' in net and 'r499_pending_score_count' in net and 'r499_score_upload_tick' in net,
 'leaderboard retry path upgraded async': '350u' in net and 'attempts>=3u' in net and 'r499_score_upload_tick' in net,
 'release tag': 'R49_9_7_RELEASE_' in build,
 'readme leaderboard timing documented': 'MM:SS.mmm' in readme and 'deterministic gameplay frames' in readme,
}
bad=[k for k,v in checks.items() if not v]
for k,v in checks.items(): print(('PASS ' if v else 'FAIL ')+k)
if bad: raise SystemExit('failed: '+', '.join(bad))
print('PASS R49.8.1 leaderboard timing/sync source invariants')
