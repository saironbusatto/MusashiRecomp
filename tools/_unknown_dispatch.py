"""Query psx-runtime for psx_unknown_dispatch hits since boot.
Default: per-target summary sorted by hit count.
With tail=N: most recent N entries from the ring."""
import socket, json, sys
def call(payload):
    s = socket.create_connection(('127.0.0.1', 4370), timeout=10)
    s.sendall((json.dumps(dict(payload, id=1))+'\n').encode())
    buf = b''
    while True:
        c = s.recv(1<<20)
        if not c: break
        buf += c
        try: return json.loads(buf.decode())
        except: pass
    raise RuntimeError("incomplete")

if len(sys.argv) > 1 and sys.argv[1] == 'tail':
    r = call({'cmd':'unknown_dispatch_log','tail':50})
    print(f"total={r.get('total')} unique={r.get('unique')} tail={r.get('tail')}")
    for e in r.get('entries', [])[-30:]:
        print(f"  seq={e['seq']:>6} addr={e['addr']} phys={e['phys']} ra={e['ra']} a0={e['a0']} a1={e['a1']} frame={e['frame']}")
else:
    r = call({'cmd':'unknown_dispatch_log'})
    print(f"=== Unknown dispatch summary ===")
    print(f"total hits={r.get('total')}  unique targets={r.get('unique')}\n")
    summary = r.get('summary', [])
    for s in summary[:30]:
        phys = s['phys']
        cnt = s['count']
        # Highlight modal-investigation PCs
        marker = ''
        for known_phys, lbl in [
            (0x1FC1A1BC, 'MODAL STUB'),
            (0x1FC1A300, 'cursor_mirror2 writer 2'),
        ]:
            if int(phys, 16) == known_phys:
                marker = f'   <-- {lbl}'
        print(f"  {phys}: {cnt:>8}{marker}")
