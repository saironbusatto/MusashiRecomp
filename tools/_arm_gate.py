"""Clear wtrace ring and arm the gate-byte range [0x7568..0x756C)."""
import socket, json

def call(d):
    s = socket.create_connection(('127.0.0.1', 4370), timeout=10)
    s.sendall((json.dumps(d)+'\n').encode())
    buf = b''
    while True:
        c = s.recv(65536)
        if not c: break
        buf += c
        if buf.endswith(b'}') or buf.endswith(b'}\n'):
            break
    s.close()
    return buf.decode().strip()

print(call({'id': 1, 'cmd': 'wtrace_clear'}))
print(call({'id': 2, 'cmd': 'wtrace_range', 'lo': '0x80007568', 'hi': '0x8000756C'}))
