"""Tally gate-byte (0x80007568..0x8000756C) write VALUES by mode (0x01/0x02/0x04/0x08/0x21).
Works against either recomp wtrace (cmd=wtrace_dump) or Beetle wtrace (cmd=beetle_wtrace).
Args: backend ('recomp' or 'beetle')."""
import socket, json, sys
from collections import Counter

backend = sys.argv[1] if len(sys.argv) > 1 else 'beetle'

def call(d):
    s = socket.create_connection(('127.0.0.1', 4370)); s.settimeout(60.0)
    s.sendall((json.dumps(d) + '\n').encode())
    buf = b''
    while True:
        try: c = s.recv(65536)
        except socket.timeout: break
        if not c: break
        buf += c
        if buf.strip().endswith(b'}'):
            try:
                json.loads(buf.decode()); break
            except Exception:
                continue
    s.close()
    return json.loads(buf.decode())

if backend == 'recomp':
    r = call({'id': 1, 'cmd': 'wtrace_dump', 'addr_lo': '0x80007568', 'addr_hi': '0x8000756C'})
    entries = r.get('entries', [])
    def mode_of(e):
        v = e['new']
        v = int(v, 16) if isinstance(v, str) else v
        return v & 0xFF
    def pc_of(e): return e['pc']
    def ra_of(e): return e['ra']
else:
    r = call({'id': 1, 'cmd': 'beetle_wtrace', 'count': 65536})
    entries = [e for e in r.get('entries', [])
               if 0x7568 <= (int(e['addr'], 16) & 0x1FFFFFFF) < 0x756C]
    def mode_of(e):
        v = int(e['val'], 16)
        return v & 0xFF
    def pc_of(e): return e['pc']
    def ra_of(e): return e['ra']

mode_counts = Counter()
mode_pcs = {}
for e in entries:
    m = mode_of(e)
    mode_counts[m] += 1
    mode_pcs.setdefault(m, Counter())[(pc_of(e), ra_of(e))] += 1

total = r.get('total', '?')
print(f"backend={backend} ring_total={total} gate_writes={len(entries)}")
for mode, n in sorted(mode_counts.items()):
    print(f"  mode 0x{mode:02X}: {n}")
    for (pc, ra), c in mode_pcs[mode].most_common(5):
        print(f"           {c:>4}× pc={pc} ra={ra}")
