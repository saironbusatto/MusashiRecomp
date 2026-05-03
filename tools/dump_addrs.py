"""Dump RAM at multiple addresses (works against either Beetle or runtime)."""
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

cmd_name = sys.argv[1] if len(sys.argv) > 1 else 'emu_read_ram'
addrs = [int(a, 16) for a in sys.argv[2:]]
if not addrs:
    addrs = [0x80080000, 0x80080040, 0x80082000, 0x80085000, 0x80087000]
for addr in addrs:
    r = call({'id':1,'cmd':cmd_name,'addr':f'0x{addr:08X}','len':64})
    print(f"  {addr:08X}: {r.get('hex', '?')}")
