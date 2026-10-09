from pathlib import Path
r=Path(__file__).resolve().parents[1]
main=(r/'src/platform/xbox/xbox_main.cpp').read_text()
net=(r/'src/platform/xbox/xbox_netplay.cpp').read_text()
hdr=(r/'src/platform/xbox/xbox_netplay.h').read_text()
inp=(r/'src/platform/xbox/xbox_input.cpp').read_text()
build=(r/'BUILD_XBOX_RELEASE.ps1').read_text()
readme=(r/'README.md').read_text()
checks={
 'cumulative R49.9 wire build': '#define ZNP_BUILD 0x5A4D490Au' in net,
 'shared system pause bit': 'R497_PAD_SYS_PAUSE 0x8000u' in main and 'j |= 0x8000u' in inp,
 'start back reserved for pause': 'START+BACK remains the deterministic shared pause' in inp,
 'hard exit moved to triggers': 'LEFT_TRIGGER' in inp and 'RIGHT_TRIGGER' in inp and 'hard_exit' in inp,
 'control packet type': 'ZNP_CONTROL = 14' in net,
 'control API': 'Xbox_Netplay_ControlSend' in hdr and 'Xbox_Netplay_ControlRecv' in hdr,
 'state gate before menu': 'gate_hash=zamn_runtime_state_hash(runtime)' in main and 'R497_Barrier(gate_seq,0' in main,
 'named save header': 'R498_SAVE_MAGIC' in main and 'R497_NAME_MAX 16' in main,
 'snapshot save': 'zamn_runtime_save_snapshot(runtime,data,cap,&used)' in main,
 'snapshot load': 'zamn_runtime_load_snapshot(runtime,st->data,st->size)' in main,
 'ROM guard': 'h.rom_hash!=s_title_rom_hash' in main,
 'serialized blob checksum': 'R497_Fnv64(data,(int)h.snapshot_size)!=h.blob_hash' in main,
 'save writes both peers': 'R497_CTRL_SAVE_COMMAND' in main and 'R497_Barrier(cmd.seq,1' in main,
 'load stages both peers': 'R497_CTRL_LOAD_COMMAND' in main and 'R497_Barrier(cmd.seq,2' in main,
 'load commit verification': 'R497_Barrier(cmd.seq,3' in main and 'pah==staged.h.state_hash' in main,
 'resume state verification': 'pf!=*io_frame||ph!=resume_hash' in main and 'fatal=true' in main,
 'failed load restores prior state': 'if(attempted&&backup.data&&!R497_ApplyStaged(runtime,&backup))fatal=true' in main,
 'timeline reset after load': 'Xbox_Netplay_ResyncAfterLoad' in main and 'znp_core_init(&s_core, local, delay)' in net,
 'rollback reset after load': 'R48_RollbackFree();' in main and 'R48_RollbackInit(runtime,Xbox_Netplay_RollbackWindow())' in main,
 'audio muted while paused': 'Xbox_Audio_SetMuted(true);' in main and 'Xbox_Audio_SetMuted(false);' in main,
 'PSO style compact keyboard': 'R497_Keys' in main and 'A TYPE  X DELETE  START DONE' in main,
 'dashboard UDATA container': 'XCreateSaveGame("U:\\\\",display,OPEN_ALWAYS' in main and 'ZAMN - Online Saves' in main,
 'release output tag': 'R49_9_7_RELEASE_' in build,
 'readme documents controls': 'START + BACK' in readme and 'Save/Load' in readme,
}
for k,v in checks.items(): print(('PASS' if v else 'FAIL'),k)
assert all(checks.values())
print('PASS R49.7 online-save source invariants')
