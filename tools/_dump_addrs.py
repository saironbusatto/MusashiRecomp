"""Dump beetle wtrace, group by addr. Args: backend [addrs...]."""
import socket, json, sys
from collections import Counter

backend = sys.argv[1] if len(sys.argv) > 1 else 'beetle'
addr_filter = set()
for a in sys.argv[2:]:
    addr_filter.add(int(a, 16) & 0x1FFFFFFF)

def call(d):
    s = socket.create_connection(("127.0.0.1", 4370), timeout=120)
    s.sendall((json.dumps(d) + "\n").encode())
    buf = b""
    while True:
        try: c = s.recv(65536)
        except socket.timeout: break
        if not c: break
        buf += c
        if buf.strip().endswith(b'}'):
            try: json.loads(buf.decode()); break
            except: continue
    s.close()
    return json.loads(buf.decode())

if backend == 'beetle':
    r = call({'id': 1, 'cmd': 'beetle_wtrace', 'count': 65536})
    entries = r.get('entries', [])
    if addr_filter:
        entries = [e for e in entries
                   if (int(e['addr'], 16) & 0x1FFFFFFF) in
                   {a & 0x1FFFFFFF for a in addr_filter}]
    print(f"ring_total={r.get('total')} returned={len(entries)}")
    by_addr = Counter()
    by_addr_pc = {}
    for e in entries:
        a = int(e['addr'], 16) & 0x1FFFFFFF
        by_addr[a] += 1
        by_addr_pc.setdefault(a, Counter())[(e['pc'], e['ra'], e['val'])] += 1
    for a, n in sorted(by_addr.items()):
        print(f"  addr 0x{a:08X}: {n} writes")
        for (pc, ra, val), c in by_addr_pc[a].most_common(8):
            print(f"           {c:>4}× val={val} pc={pc} ra={ra}")
else:
    addr_filter_strs = sys.argv[2:] if len(sys.argv) > 2 else ['0x80007514']
    for a in addr_filter_strs:
        lo = int(a, 16) & 0xFFFFFFFF
        hi = lo + 4
        r = call({'id': 1, 'cmd': 'wtrace_dump',
                  'addr_lo': f'0x{lo:08X}', 'addr_hi': f'0x{hi:08X}'})
        entries = r.get('entries', [])
        print(f"\n--- recomp wtrace_dump 0x{lo:08X}..0x{hi:08X} ---")
        print(f"ring_total={r.get('total')} entries={len(entries)}")
        by_pc = Counter()
        for e in entries:
            by_pc[(e['pc'], e['ra'], e['old'], e['new'])] += 1
        for (pc, ra, old, new), c in by_pc.most_common(20):
            print(f"  {c:>4}× {old:>10}->{new:>10} pc={pc} ra={ra}")
