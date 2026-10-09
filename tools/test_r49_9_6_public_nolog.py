from pathlib import Path

root = Path(__file__).resolve().parents[1]
build = (root / "BUILD_XBOX_RELEASE.ps1").read_text(encoding="utf-8")
main = (root / "src/platform/xbox/xbox_main.cpp").read_text(encoding="utf-8")
hdr = (root / "src/platform/xbox/xbox_platform.h").read_text(encoding="utf-8")
enabled = build[build.index("foreach ($d in @("):build.index("    )) { if ($defs -notcontains $d)")]
readme = (root / "README.md").read_text(encoding="utf-8")

checks = {
    "R49.9.6 release tag": "R49_9_7_RELEASE_" in build,
    "release no-diagnostics define": "'ZAMN_RELEASE_NO_DIAGNOSTICS=1'" in build,
    "buffered logger not enabled": "'ZAMN_R39_BUFFERED_LOG=1'" not in enabled,
    "service logger not enabled": "'ZAMN_R499_SERVICE_LOG=1'" not in enabled,
    "release logger compiles out at call sites": "#define Xbox_Log(...) ((void)0)" in hdr,
    "release flush compiles out at call sites": "#define Xbox_LogFlush() ((void)0)" in hdr,
    "R49.9.6 boot marker": "R49.9.6 PUBLIC-NOLOG" in main,
    "README states logging disabled": "runtime logging disabled" in readme.lower(),
    "netplay preserved": "'ZAMN_R47_NETPLAY=1'" in build,
    "rollback preserved": "'ZAMN_R48_ROLLBACK=1'" in build,
}
failed=False
for name, ok in checks.items():
    print(("PASS " if ok else "FAIL ") + name)
    failed |= not ok
raise SystemExit(1 if failed else 0)
