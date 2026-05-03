"""Dump RAM as 4-byte words. Args: addr_hex len_bytes [backend]"""
import socket, json, sys, struct
addr = sys.argv[1]
length = int(sys.argv[2])
backend = sys.argv[3] if len(sys.argv) > 3 else 'beetle'
cmd = 'emu_read_ram' if backend == 'beetle' else 'read_ram'

s = socket.create_connection(("127.0.0.1", 4370), timeout=30)
s.sendall((json.dumps({"id":1,"cmd":cmd,"addr":addr,"len":length}) + "\n").encode())
buf = b""
while True:
    try:
        c = s.recv(65536)
    except socket.timeout:
        break
    if not c: break
    buf += c
    if buf.strip().endswith(b'}'):
        try:
            json.loads(buf.decode())
            break
        except Exception:
            continue
r = json.loads(buf.decode())
hx = r.get('hex', '')
b = bytes.fromhex(hx)
base = int(addr, 16)
for i in range(0, len(b), 4):
    if i+4 > len(b): break
    word = struct.unpack("<I", b[i:i+4])[0]
    print(f"  0x{base+i:08X}: 0x{word:08X}")
