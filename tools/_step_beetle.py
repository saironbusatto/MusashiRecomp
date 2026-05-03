"""Step Beetle a chunk of frames."""
import socket, json, sys
n = int(sys.argv[1]) if len(sys.argv) > 1 else 1500
s = socket.create_connection(("127.0.0.1", 4370), timeout=120)
s.sendall((json.dumps({"id":1,"cmd":"emu_step","count":n}) + "\n").encode())
buf = b""
while True:
    c = s.recv(65536)
    if not c: break
    buf += c
    if buf.strip().endswith(b'}'):
        try: json.loads(buf.decode()); break
        except: continue
print(buf.decode())
