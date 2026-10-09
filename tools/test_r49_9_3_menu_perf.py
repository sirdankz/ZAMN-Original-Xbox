from pathlib import Path
root=Path(__file__).resolve().parents[1]
net=(root/'src/platform/xbox/xbox_netplay.cpp').read_text(errors='ignore')
main=(root/'src/platform/xbox/xbox_main.cpp').read_text(errors='ignore')
build=(root/'BUILD_XBOX_RELEASE.ps1').read_text(errors='ignore')
readme=(root/'README.md').read_text(errors='ignore')
checks={
 'wire build unchanged':'#define ZNP_BUILD 0x5A4D490Au' in net,
 'R49.9.3 boot marker':'R49.9.3 ASYNC-MENUS/WARM-NET/PACKED-BOARDS' in main,
 'warm network stack':'ensure_network_stack' in net and 'network stack warm' in net,
 'transport close does not cleanup stack':'WSACleanup' not in net[net.index('static void close_transport(void){'):net.index('bool Xbox_Netplay_Init',net.index('static void close_transport(void){'))] and 'XNetCleanup' not in net[net.index('static void close_transport(void){'):net.index('bool Xbox_Netplay_Init',net.index('static void close_transport(void){'))],
 'cached local ip':'s_local_ip_cached' in net and 'local IPv4 cached' in net,
 'cached public ip':'s_public_ip_cached' in net and 'public IP cache hit' in net,
 'packed leaderboard request':'LISTP' in net and 'LISTD' in net,
 'packed leaderboard response':'ZLB2|PACK|' in net and 'r499_parse_board_pack' in net,
 'board RAM cache':'s_board_cache[2][4][56]' in net,
 'board asynchronous poll':'r499_board_async_tick' in net and 'r499_board_async_start' in net,
 'scroll debounce':'180u' in net,
 'no old blocking board fetch':'static int r498_fetch_board' not in net,
 'no multipart retry storm':'partial-retry' not in net,
 'async score upload':'r499_score_upload_tick' in net and 'SCORE async-ack' in net,
 'leaderboard labels preserved':'SOLO ONLINE LEADERBOARDS' in net and 'CO-OP ONLINE LEADERBOARDS' in net,
 'release tag':'R49_9_7_RELEASE_' in build,
 'README documents responsive menus':'responsive asynchronous online menus' in readme,
}
bad=[]
for k,v in checks.items():
 print(('PASS ' if v else 'FAIL ')+k)
 if not v: bad.append(k)
if bad: raise SystemExit('failed: '+', '.join(bad))
print('PASS R49.9.3 menu performance source invariants')
