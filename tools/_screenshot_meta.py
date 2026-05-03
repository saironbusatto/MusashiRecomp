"""Probe screenshot command on whichever side is alive."""
import socket, json, sys
port = int(sys.argv[1]) if len(sys.argv) > 1 else 4370
cmd  = sys.argv[2] if len(sys.argv) > 2 else 'screenshot'
s = socket.create_connection(("127.0.0.1", port), timeout=15)
s.sendall((json.dumps({"id":1,"cmd":cmd}) + "\n").encode())
buf = b""
while True:
    c = s.recv(65536)
    if not c: break
    buf += c
    if len(buf) > 16 and (buf.endswith(b"}\n") or buf.endswith(b"}")): break
try:
    r = json.loads(buf.decode())
    print({k: (v if not isinstance(v, str) or len(v) < 80 else f"<{len(v)} chars>") for k, v in r.items()})
except Exception as e:
    print('decode error:', e, 'len=', len(buf))
    print(buf[:300])
