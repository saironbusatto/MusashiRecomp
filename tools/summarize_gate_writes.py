"""Summarize all writes to gate addresses, flagging values."""
import socket, json, sys
from collections import Counter

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

which = sys.argv[1] if len(sys.argv) > 1 else 'recomp'

if which == 'recomp':
    r = call({'id':1,'cmd':'wtrace_dump','addr_lo':'0x80007568','addr_hi':'0x8000756C'})
    entries = r.get('entries', [])
else:
    r = call({'id':1,'cmd':'beetle_wtrace','count':'14000'})
    entries = [e for e in r.get('entries', [])
               if 0x7568 <= int(e['addr'], 16) < 0x756C]

print(f"total entries: {len(entries)}")

# Tally by (value, func, ra, pc)
by_val_writer = Counter()
for e in entries:
    new = e.get('new', e.get('val', '?'))
    func = e.get('func', '?')
    ra = e.get('ra', '?')
    pc = e.get('pc', '?')  # exact store-instruction PC (recomp only)
    by_val_writer[(str(new), str(func), str(ra), str(pc))] += 1

print()
print(f"writers (value,func,ra,pc) -> count:")
for (val, func, ra, pc), n in by_val_writer.most_common():
    print(f"  {val} fn={func} ra={ra} pc={pc} : {n}")
