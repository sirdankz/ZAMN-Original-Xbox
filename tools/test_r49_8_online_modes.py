from pathlib import Path
r=Path(__file__).resolve().parents[1]
main=(r/'src/platform/xbox/xbox_main.cpp').read_text()
net=(r/'src/platform/xbox/xbox_netplay.cpp').read_text()
hdr=(r/'src/platform/xbox/xbox_netplay.h').read_text()
inp=(r/'src/platform/xbox/xbox_input.cpp').read_text()
title=(r/'src/platform/xbox/xbox_title_online.h').read_text()
runtime=(r/'src/native/runtime.c').read_text()
build=(r/'BUILD_XBOX_RELEASE.ps1').read_text()
readme=(r/'README.md').read_text()
checks={
 'cumulative R49.9 wire build': '#define ZNP_BUILD 0x5A4D490Au' in net,
 'four rulesets': all(x in hdr for x in ['ZAMN_RULE_NORMAL_SAVE','ZAMN_RULE_NORMAL_NOSAVE','ZAMN_RULE_HARDCORE_SAVE','ZAMN_RULE_HARDCORE_NOSAVE']),
 'session exit bit': 'R498_PAD_SYS_EXIT 0x4000u' in main and 'j |= 0x4000u' in inp,
 'right stick exit chord': 'XINPUT_GAMEPAD_RIGHT_THUMB' in inp and 'session_exit' in inp,
 'online title solo/profile': 'SOLO PLAY' in main and 'PROFILE' in main and 'return 5' in title and 'return 6' in title,
 'profiles persisted UDATA': 'ZAMN - Online Profile' in net and 'profiles-r499.dat' in net,
 'profile handshake': 's_peer_profile' in net and 'payload[16]=s_ruleset' in net and 'payload[17]' in net,
 'public ruleset advertisement': 'ZDIR1|REGISTER|%08lX|%08lX%08lX|%u|' in net and 'ROOM|%11[^|]|%31[^|]|%u|%u|%u' in net,
 'leaderboard protocol upgraded': 'ZLB2|SUBMIT|' in net and 'LISTP' in net and 'LISTD' in net and 'GLOBAL TOP 5' in net,
 'host-selectable rollback policy': 'ZMENU_RS' in net and 'rollback_mode=!rollback_mode' in net and 'ROLLBACK: %s' in net and 'negotiated_pair(delay_option, auto_delay, rollback_mode' in net,
 'rollback-safe ruleset metadata': 'R498PendingScore' in main and 'slot->clock = s_r498_clock' in main and 'R498_PendingScoresDiscardFrom(frame)' in main and 'R498_CommitAuthoritativeScores();' in main,
 'leaderboard retained': 'Xbox_Netplay_LeaderboardMenu' in net,
 'deterministic level address': '0x1e7cu' in runtime and 'zamn_runtime_current_level' in runtime,
 'lives and health addresses': '0x1d4cu' in runtime and '0x1cb8u' in runtime,
 'normal five-level reward': '(completed%5u)==0u' in main and 'zamn_runtime_normal_checkpoint_reward(runtime,players)' in main,
 'solo only one player reward': 'const int players=Xbox_Netplay_Active()?2:1' in main,
 'save aware timer': 'level_frames' in main and 'h.ruleset' in main and 'clock->frames=staged.h.level_frames' in main,
 'no-save gating': 'Xbox_Netplay_RulesetSavable(ruleset)' in main and 'NO-SAVE RULESET' in main,
 'solo save menu': 'R498_RunSoloSaveMenu' in main,
 'release tag': 'R49_9_7_RELEASE_' in build,
 'readme R49.8 modes': 'Normal / Savable' in readme and 'Hardcore / No Save' in readme and 'Right Stick Click' in readme,
}
for k,v in checks.items(): print(('PASS' if v else 'FAIL'),k)
assert all(checks.values())
print('PASS R49.8 modes/profile/leaderboard source invariants')
