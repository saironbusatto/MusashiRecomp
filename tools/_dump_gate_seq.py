"""Dump gate-byte writes [0x7568..0x756C) in seq order with pc, frame, value.
Args: [recomp|beetle]"""
import socket, json, sys

backend = sys.argv[1] if len(sys.argv) > 1 else 'recomp'

def call(d):
    s = socket.create_connection(('127.0.0.1', 4370), timeout=120)
    s.sendall((json.dumps(d)+'\n').encode())
    buf = b''
    depth = 0; in_str = False; esc = False; started = False
    while True:
        c = s.recv(65536)
        if not c: break
        buf += c
        for b in c:
            if esc: esc = False; continue
            if b == 0x5C: esc = True; continue
            if b == 0x22: in_str = not in_str; continue
            if in_str: continue
            if b == 0x7B: depth += 1; started = True
            elif b == 0x7D: depth -= 1
        if started and depth == 0: break
    s.close()
    return json.loads(buf.decode())

if backend == 'recomp':
    r = call({'id': 1, 'cmd': 'wtrace_dump',
              'addr_lo': '0x80007568', 'addr_hi': '0x8000756C'})
    entries = r.get('entries', [])
    print(f"total={r.get('total')} avail={r.get('available')} entries={len(entries)}")
    for e in entries:
        print(f"  fr={e['frame']:>6} seq={e['seq']:>6} {e['addr']} <- {e['new']:>10} "
              f"(was {e['old']:>10}) pc={e['pc']} fn={e['func']} ra={e['ra']} w={e['w']}")
elif backend == 'step':
    n = int(sys.argv[2]) if len(sys.argv) > 2 else 1500
    print('step:', call({'id':1,'cmd':'emu_step','count':n}).get('oracle_frame'))
    print('frame:', call({'id':2,'cmd':'ping'}).get('frame'))
else:
    r = call({'id': 1, 'cmd': 'beetle_wtrace', 'count': 65536})
    entries = [e for e in r.get('entries', [])
               if 0x7568 <= (int(e['addr'], 16) & 0x1FFFFFFF) < 0x756C]
    print(f"ring_total={r.get('total')} returned={len(r.get('entries', []))} "
          f"gate_writes={len(entries)}")
    for e in entries:
        print(f"  fr={e['frame']:>6} seq={e['seq']:>6} {e['addr']} <- {e['val']:>10} "
              f"pc={e['pc']} ra={e['ra']} sz={e['size']}")
