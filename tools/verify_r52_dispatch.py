"""Fail if R52 whitelist drifts away from the real thread dispatcher."""
from pathlib import Path
import re
root=Path(__file__).resolve().parents[1]
collide=(root/'src/port/collide.c').read_text()
body=collide[collide.index('bool thread_call_handler_counted'):collide.index('bool thread_call_handler(',collide.index('bool thread_call_handler_counted'))]
actual=set(re.findall(r'entry\s*==\s*([A-Z][A-Z0-9_]*ENTRY)',body))
header=(root/'src/port/r52_fast_guard.h').read_text()
start=header[header.index('static inline bool r52_handler_recognized'):header.index('static inline R52GuardDecision r52_handler_slot_decision')]
whitelist=set(re.findall(r'case\s+([A-Z][A-Z0-9_]*ENTRY)\s*:',start))
if actual!=whitelist:
    raise SystemExit(f'FAIL whitelist mismatch: missing={sorted(actual-whitelist)} stale={sorted(whitelist-actual)}')
print(f'PASS R52 whitelist matches all {len(actual)} exact port dispatcher handler addresses')
