"""Ping debug server (recomp or beetle)."""
import socket, json, sys
port = int(sys.argv[1]) if len(sys.argv) > 1 else 4370
s = socket.create_connection(("127.0.0.1", port), timeout=5)
s.sendall(b'{"id":1,"cmd":"ping"}\n')
buf = b""
while True:
    c = s.recv(4096)
    if not c: break
    buf += c
    if b"}" in buf: break
print(buf.decode().strip())
