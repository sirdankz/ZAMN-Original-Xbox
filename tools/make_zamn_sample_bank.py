#!/usr/bin/env python3
"""Build an optional ZAMN remastered PCM sample bank (ZSB1).

Input files are named 00.wav .. FF.wav by the SNES DSP SRCN value they
replace. Missing IDs intentionally fall back to the original BRR sample.
An optional loops.json maps hex IDs to [loop_start_frame, loop_end_frame].

WAV input: PCM 16-bit mono or stereo, 4-96 kHz. Stereo is downmixed to mono.
"""
from __future__ import annotations
import argparse, json, struct, wave
from pathlib import Path

COUNT=256
ENTRY=20
HEADER=16
TABLE_END=HEADER+COUNT*ENTRY

def read_wav(path: Path):
    with wave.open(str(path), 'rb') as w:
        if w.getcomptype() != 'NONE' or w.getsampwidth() != 2:
            raise ValueError(f"{path.name}: require uncompressed 16-bit PCM WAV")
        ch=w.getnchannels(); rate=w.getframerate(); frames=w.getnframes()
        if ch not in (1,2): raise ValueError(f"{path.name}: require mono/stereo")
        if not 4000 <= rate <= 96000: raise ValueError(f"{path.name}: rate {rate} unsupported")
        raw=w.readframes(frames)
    vals=struct.unpack('<'+'h'*(len(raw)//2), raw)
    if ch==2:
        mono=[]
        for i in range(0,len(vals),2):
            v=(int(vals[i])+int(vals[i+1]))//2
            mono.append(max(-32768,min(32767,v)))
        vals=mono
    return rate, list(vals)

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('input_dir', type=Path)
    ap.add_argument('output', type=Path)
    ap.add_argument('--loops', type=Path, default=None,
                    help='JSON mapping hex SRCN to [start_frame,end_frame]')
    a=ap.parse_args()
    loops={}
    lp=a.loops or (a.input_dir/'loops.json')
    if lp.exists(): loops=json.loads(lp.read_text(encoding='utf-8'))

    entries=[(0,0,0,0,0)]*COUNT
    payload=bytearray()
    used=0
    for i in range(COUNT):
        p=a.input_dir/f'{i:02X}.wav'
        if not p.exists():
            p=a.input_dir/f'{i:02x}.wav'
        if not p.exists(): continue
        rate, pcm=read_wav(p)
        start=end=0
        spec=loops.get(f'{i:02X}', loops.get(f'{i:02x}'))
        if spec is not None:
            if not isinstance(spec,list) or len(spec)!=2:
                raise ValueError(f'loop {i:02X}: expected [start,end]')
            start,end=map(int,spec)
            if not (0 <= start < end <= len(pcm)):
                raise ValueError(f'loop {i:02X}: out of range')
        offset=TABLE_END+len(payload)
        data=struct.pack('<'+'h'*len(pcm), *pcm)
        payload += data
        entries[i]=(offset,len(pcm),start,end,rate)
        used += 1

    out=bytearray(b'ZSB1')+struct.pack('<III',1,COUNT,ENTRY)
    for e in entries: out += struct.pack('<IIIII',*e)
    out += payload
    a.output.parent.mkdir(parents=True, exist_ok=True)
    a.output.write_bytes(out)
    print(f'ZSB1: {a.output} | samples={used} | bytes={len(out)}')

if __name__=='__main__': main()
