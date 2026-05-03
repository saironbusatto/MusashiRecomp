"""Arm wtrace on 0x80080000..0x80088000 then dump."""
import socket, json, sys

def call(d):
    s = socket.create_connection(('127.0.0.1', 4370)); s.settimeout(15.0)
    s.sendall((json.dumps(d) + '\n').encode())
    buf = b''
    while True:
        try: c = s.recv(65536)
        except socket.timeout: break
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

cmd = sys.argv[1] if len(sys.argv) > 1 else 'arm'

if cmd == 'arm':
    # Recomp uses lo/hi as hex strings
    print(call({'id':1,'cmd':'wtrace_clear'}))
    print(call({'id':2,'cmd':'wtrace_add','lo':'0x80080000','hi':'0x80088000'}))
    print(call({'id':3,'cmd':'wtrace_ranges'}))
elif cmd == 'arm-beetle':
    print(call({'id':1,'cmd':'beetle_wtrace_disarm'}))
    print(call({'id':2,'cmd':'beetle_wtrace_arm','lo':'0x80080000','hi':'0x80088000'}))
    # Keep the gate-byte arm too
    print(call({'id':3,'cmd':'beetle_wtrace_arm','lo':'0x80007568','hi':'0x8000756C'}))
    print(call({'id':4,'cmd':'beetle_wtrace_ranges'}))
elif cmd == 'dump':
    r = call({'id':1,'cmd':'wtrace_dump','count':'2000'})
    print(f"total={r.get('total','?')} returned={r.get('count','?')}")
    for e in r.get('entries', []):
        addr = e.get('addr', '?')
        val = e.get('value', e.get('val', '?'))
        pc = e.get('pc', '?')
        ra = e.get('ra', '?')
        sz = e.get('size', '?')
        fr = e.get('frame', '?')
        print(f'  fr={fr:>6} {addr}={val} pc={pc} ra={ra} sz={sz}')
elif cmd == 'dump-beetle':
    r = call({'id':1,'cmd':'beetle_wtrace','count':'2000'})
    print(f"total={r.get('total','?')} returned={r.get('count','?')}")
    for e in r.get('entries', []):
        print(f'  fr={e["frame"]:>6} {e["addr"]}={e["val"]} pc={e["pc"]} ra={e["ra"]} sz={e["size"]}')
