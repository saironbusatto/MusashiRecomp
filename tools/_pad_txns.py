"""Show recent SIO trace entries — looking for pad polling activity."""
import socket, json
s = socket.create_connection(('127.0.0.1', 4370), timeout=10)
s.sendall((json.dumps({'id':1,'cmd':'sio_trace_dump','count':40}) + '\n').encode())
buf = b''
while True:
    c = s.recv(65536)
    if not c: break
    buf += c
    if buf.strip().endswith(b'}'):
        try: json.loads(buf.decode()); break
        except: continue
print(buf.decode()[:4000])
