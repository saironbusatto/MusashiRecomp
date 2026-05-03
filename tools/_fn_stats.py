"""Print fn_trace stats."""
import socket, json
s = socket.create_connection(('127.0.0.1', 4370), timeout=10)
s.sendall(b'{"id":1,"cmd":"fn_stats"}\n')
buf = b''
while True:
    c = s.recv(4096)
    buf += c
    if buf.strip().endswith(b'}'):
        try: json.loads(buf.decode()); break
        except: continue
print(buf.decode())
