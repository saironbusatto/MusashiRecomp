"""Decode ASCII strings at given addresses."""
import socket, json, sys

def read(addr, n=24):
    s = socket.create_connection(('127.0.0.1', 4370), timeout=10)
    s.sendall((json.dumps({'id': 1, 'cmd': 'read_ram', 'addr': addr, 'len': n}) + '\n').encode())
    b = b''
    while True:
        c = s.recv(8192)
        if not c: break
        b += c
        try: json.loads(b.decode()); break
        except: continue
    r = json.loads(b.decode())
    return bytes.fromhex(r['hex'])

addrs = sys.argv[1:] if len(sys.argv) > 1 else ['0x80066B94', '0x80066B9C', '0x80066BA4', '0x80066BB0']
for a in addrs:
    data = read(a, 32)
    end = data.find(b'\x00')
    if end < 0: end = len(data)
    s = data[:end].decode('latin-1', errors='replace')
    print(f"  {a}: {data[:end].hex()} = {s!r}")
