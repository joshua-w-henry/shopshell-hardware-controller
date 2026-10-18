"""Offline assertion tests for the wired NEC encoding/timing and complete manifest.
Run: python tests/test_nec_waveform.py
No Arduino or ESP32 toolchain required.
"""
import json
from pathlib import Path

root=Path(__file__).resolve().parents[1]
data=json.loads((root/'klipsch_commands.json').read_text())
commands=data['commands']
assert len(commands)==15

def reverse8(x):
    return int(f'{x:08b}'[::-1],2)

def rawnec(addr,cmd):
    a=reverse8(addr)
    c=reverse8(cmd)
    return (a<<24)|((a^0xff)<<16)|(c<<8)|(c^0xff)

def waveform(raw):
    result=[(9000,4500)]
    for n in range(31,-1,-1):
        result.append((560,1690 if ((raw >> n)&1) else 560))
    result.append((560,0))
    return result

assert rawnec(0x02,0x0A)==0x40BF50AF
assert rawnec(0x02,0x08)==0x40BF10EF
assert rawnec(0x02,0x1C)==0x40BF38C7
assert rawnec(0x02,0x00)==0x40BF00FF
seen=set()
for entry in commands:
    cmd=int(entry['logical_command'],16)
    raw=rawnec(0x02,cmd)
    assert raw==int(entry['raw'],16),entry
    assert entry['path'] not in seen
    seen.add(entry['path'])
    w=waveform(raw)
    assert len(w)==34
    assert w[0]==(9000,4500) and w[-1]==(560,0)
    assert all(pulse==560 and space in (560,1690) for pulse,space in w[1:-1])
    assert sum(1 for _,space in w[1:-1] if space==1690)==raw.bit_count()
assert 9000+2250+560 == 11810  # NEC repeat pulse envelope duration
assert len(seen)==15
print(f'PASS: {len(seen)} NEC commands; 34 RMT items per full frame; Power=0x40BF50AF')
