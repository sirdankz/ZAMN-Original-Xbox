from pathlib import Path

root = Path(__file__).resolve().parents[1]
main = (root / 'src/platform/xbox/xbox_main.cpp').read_text(encoding='utf-8')
hdr = (root / 'src/platform/xbox/xbox_platform.h').read_text(encoding='utf-8')
build = (root / 'BUILD_XBOX_RELEASE.ps1').read_text(encoding='utf-8')
enabled = build[build.index('foreach ($d in @('):build.index('    )) { if ($defs -notcontains $d)')]

checks = {
    'release no-log branch exists': '#if defined(ZAMN_RELEASE_NO_DIAGNOSTICS) && !defined(ZAMN_R499_SERVICE_LOG)' in main,
    'release log calls compile out at call sites': '#define Xbox_Log(...) ((void)0)' in hdr and '#define Xbox_LogFlush() ((void)0)' in hdr,
    'release macro still skips PERF block': '#ifndef ZAMN_RELEASE_NO_DIAGNOSTICS\n    if ((frame % PERF_WINDOW) == 0)' in main,
    'release macro still skips per-frame timer': '#ifndef ZAMN_RELEASE_NO_DIAGNOSTICS\n    DWORD t0 = GetTickCount();' in main,
    'release macro still skips cycle counter': '#ifndef ZAMN_RELEASE_NO_DIAGNOSTICS\n    const uint64_t c0 = __builtin_readcyclecounter();' in main,
    'release build enables no-heavy-diagnostics': "'ZAMN_RELEASE_NO_DIAGNOSTICS=1'" in build,
    'release build disables buffered event logger': "'ZAMN_R39_BUFFERED_LOG=1'" not in enabled,
    'release build disables service logger': "'ZAMN_R499_SERVICE_LOG=1'" not in enabled,
    'release build does not enable residual sampler': 'ZAMN_R291_HOT_RESIDUE=1' not in build,
    'release build does not enable light profiler': "'ZAMN_R20_LIGHT_PROF=1'" not in build,
    'release build keeps netplay': "'ZAMN_R47_NETPLAY=1'" in build,
    'release build keeps rollback code': "'ZAMN_R48_ROLLBACK=1'" in build,
    'release packages named console folder': "'Zombies Ate My Neighbors!'" in build,
}

failed = False
for name, ok in checks.items():
    print(('PASS ' if ok else 'FAIL ') + name)
    failed |= not ok
raise SystemExit(1 if failed else 0)
