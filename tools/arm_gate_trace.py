"""Arm comprehensive gate-trace ranges."""
import socket, json, sys

def call(d):
    s = socket.create_connection(('127.0.0.1', 4370)); s.settimeout(15.0)
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

target = sys.argv[1] if len(sys.argv) > 1 else 'beetle'

ranges = [
    ('0x80007568', '0x8000756C'),  # gates (4 slots)
    ('0x80007550', '0x8000755C'),  # per-slot chain handlers
    ('0x800075C0', '0x800075C8'),  # state pointer area
    ('0x80007264', '0x80007268'),  # slot toggle
    ('0x80007520', '0x80007524'),  # chain-restart discriminator (recomp=0, beetle!=0)
    ('0x80007514', '0x80007518'),  # FP-table counter (mem[0x7514])
    ('0x80007528', '0x80007538'),  # per-slot FP table (mem[0x7528..0x7537])
    ('0x80007258', '0x80007260'),  # struct pointers used by func_00004D6C
]

if target == 'beetle':
    print(call({'id':1,'cmd':'beetle_wtrace_disarm'}))
    for lo, hi in ranges:
        r = call({'id':1,'cmd':'beetle_wtrace_arm','lo':lo,'hi':hi})
        print(f'  arm {lo}..{hi}: slot={r.get("slot","?")}')
    print(call({'id':1,'cmd':'beetle_wtrace_reset'}))
    print(call({'id':1,'cmd':'beetle_wtrace_ranges'}))
else:
    # Recomp uses wtrace_add (additive — clear first)
    print(call({'id':1,'cmd':'wtrace_clear'}))
    for lo, hi in ranges:
        r = call({'id':1,'cmd':'wtrace_add','lo':lo,'hi':hi})
        print(f'  arm {lo}..{hi}: slot={r.get("slot","?")}')
    print(call({'id':1,'cmd':'wtrace_ranges'}))
