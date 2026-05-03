"""Dump Beetle wtrace, filter writes to MainRAM[0x7520..0x7524)."""
import socket, json, sys

def call(d):
    s = socket.create_connection(('127.0.0.1', 4370)); s.settimeout(60.0)
    s.sendall((json.dumps(d) + '\n').encode())
    buf = b''
    while True:
        try: c = s.recv(65536)
        except socket.timeout: break
        if not c: break
        buf += c
        depth = 0; ins = False; esc = False
        for b in buf:
            if esc: esc = False; continue
            if b == 0x5C: esc = True; continue
            if b == 0x22: ins = not ins; continue
            if ins: continue
            if b == 0x7B: depth += 1
            elif b == 0x7D: depth -= 1
        if depth == 0 and buf.strip(): break
    s.close()
    return json.loads(buf.decode())

count = int(sys.argv[1]) if len(sys.argv) > 1 else 65536
r = call({'id': 1, 'cmd': 'beetle_wtrace', 'count': count})
total = r.get('total', '?')
got = len(r.get('entries', []))
hits = []
for e in r.get('entries', []):
    addr = int(e['addr'], 16) & 0x1FFFFFFF
    if 0x7520 <= addr < 0x7524:
        hits.append(e)
print(f"total ring entries={total} returned={got} hits[0x7520..0x7524)={len(hits)}")
for e in hits[:300]:
    print(f"  seq={e['seq']:>6} fr={e['frame']:>5} {e['addr']}={e['val']} "
          f"pc={e['pc']} ra={e['ra']} sz={e['size']}")
