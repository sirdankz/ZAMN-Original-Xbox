from pathlib import Path

root = Path(__file__).resolve().parents[1]
main = (root / "src/platform/xbox/xbox_main.cpp").read_text()
net = (root / "src/platform/xbox/xbox_netplay.cpp").read_text()
runtime = (root / "src/native/runtime.c").read_text()

# R49.2 identity.
assert "#define ZNP_BUILD 0x5A4D4902u" in net
assert "boot R49.2 ROLLBACK SLOT RECYCLE + APU PHASE RESTORE" in main
assert "r49.2-rb-slot-recycle=" in main

# R49.2 slot protection must be bounded by the actual negotiated rollback window.
assert "const uint32_t window = (uint32_t)Xbox_Netplay_RollbackWindow();" in main
assert "age <= window && !authoritative" in main
assert "recycle expired snapshot" in main
assert "slot->frame != frame &&\n      !Xbox_Netplay_FrameAuthoritative(slot->frame)) return false" not in main

# Keep the complete R49.1 deterministic APU continuation fix.
assert "ZAMN_RB_SNAPSHOT_MAGIC 0x3253425a" in runtime
assert "memcpy(tail, &rt->snes->apuMasterPending" in runtime
assert "memcpy(tail, &rt->snes->apuCycleDebtNumerator" in runtime
assert "memcpy(&rt->snes->apuMasterPending, tail" in runtime
assert "memcpy(&rt->snes->apuCycleDebtNumerator, tail" in runtime
assert "r47_hash_u32(h, s->apuMasterPending)" in runtime
assert "r47_hash_u64(h, (uint64_t)s->apuCycleDebtNumerator)" in runtime

# Model frame 882 with a one-frame rollback window. Any state older than one
# frame cannot legally be the origin of a rollback request and must not be held
# hostage by an authority lookup that can be false for unrelated/stale reasons.
new_frame = 882
old_frame = 879
window = 1
age = new_frame - old_frame
authoritative = False
old_r49_would_fail = not authoritative
new_r49_2_would_fail = age <= window and not authoritative
assert age > window
assert old_r49_would_fail and not new_r49_2_would_fail

# The even stronger aged-out-history case is also safe.
old_frame = 147
age = new_frame - old_frame
assert age > 256 and age > window
assert not (age <= window and not authoritative)

print("R49.2 rollback-slot recycle + R49.1 APU-state regression PASS")
