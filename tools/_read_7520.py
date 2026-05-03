"""Sample mem[0x80007520] from whichever side is alive."""
import socket, json, sys
port = int(sys.argv[1]) if len(sys.argv) > 1 else 4370
backend = sys.argv[2] if len(sys.argv) > 2 else 'beetle'   # 'beetle' or 'recomp'
cmd = 'emu_read_ram' if backend == 'beetle' else 'read_ram'
def call(d):
    s = socket.create_connection(("127.0.0.1", port), timeout=10)
    s.sendall((json.dumps(d)+'\n').encode())
    buf = b""
    while True:
        c = s.recv(4096)
        if not c: break
        buf += c
        if b'}' in buf: break
    return json.loads(buf.decode())
for off, name in [(0x7520, '0x7520'), (0x755A, '0x755A'), (0x7568, '0x7568')]:
    addr = f"0x{0x80000000 + off:08X}"
    r = call({'id': 1, 'cmd': cmd, 'addr': addr, 'len': 4})
    print(f"{name}@{addr} = {r.get('hex','?')}")
