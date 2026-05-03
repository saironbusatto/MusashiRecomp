"""Summarize wtrace by writer function PC."""
import socket, json, sys
from collections import Counter, defaultdict

def call(d):
    s = socket.create_connection(('127.0.0.1', 4370)); s.settimeout(60.0)
    s.sendall((json.dumps(d) + '\n').encode())
    buf = b''
    while True:
        try: c = s.recv(65536)
        except: break
        if not c: break
        buf += c
        depth=0; ins=False; esc=False
        for b in buf:
            if esc: esc=False; continue
            if b==0x5C: esc=True; continue
            if b==0x22: ins=not ins; continue
            if ins: continue
            if b==0x7B: depth+=1
            elif b==0x7D: depth-=1
        if depth==0 and buf.strip(): break
    s.close()
    return json.loads(buf.decode())

lo = sys.argv[1] if len(sys.argv) > 1 else '0x80080000'
hi = sys.argv[2] if len(sys.argv) > 2 else '0x80088000'
which = sys.argv[3] if len(sys.argv) > 3 else 'recomp'

if which == 'recomp':
    r = call({'id':1,'cmd':'wtrace_dump','addr_lo':lo,'addr_hi':hi})
    entries = r.get('entries', [])
else:
    r = call({'id':1,'cmd':'beetle_wtrace','count':'65536'})
    entries = [e for e in r.get('entries', [])
               if int(e['addr'], 16) >= int(lo, 16) and int(e['addr'], 16) < int(hi, 16)]

print(f"total entries: {len(entries)}")

# Group by func/ra
by_func_ra = Counter()
for e in entries:
    key = (e.get('func', '?'), e.get('ra', '?'))
    by_func_ra[key] += 1
print()
print(f"writers (func,ra) -> count:")
for (func, ra), n in by_func_ra.most_common():
    print(f"  func={func} ra={ra} : {n}")

# Address-region summary
print()
addr_set = set(int(e['addr'], 16) for e in entries)
print(f"unique addresses written: {len(addr_set)}")
print(f"min addr: 0x{min(addr_set):08X}")
print(f"max addr: 0x{max(addr_set):08X}")

# First and last writes
print()
print("first 10 writes:")
for e in entries[:10]:
    print(f"  fr={e['frame']:>5} {e['addr']} {e.get('old','?')}->{e.get('new', e.get('val','?'))} func={e.get('func','?')} ra={e['ra']} sz={e.get('w', e.get('size','?'))}")
print()
print("last 10 writes:")
for e in entries[-10:]:
    print(f"  fr={e['frame']:>5} {e['addr']} {e.get('old','?')}->{e.get('new', e.get('val','?'))} func={e.get('func','?')} ra={e['ra']} sz={e.get('w', e.get('size','?'))}")
