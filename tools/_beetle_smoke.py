"""Smoke test psx-beetle wire protocol on port 4380."""
import socket, json, sys
s = socket.create_connection(('127.0.0.1', 4380), timeout=5)
def call(p):
    s.sendall((json.dumps(p)+'\n').encode())
    buf = b''
    while True:
        c = s.recv(1<<16)
        if not c: break
        buf += c
        if buf.endswith(b'\n'): break
    return json.loads(buf.decode())
print('PING  :', call({'id':1,'cmd':'ping'}))
print('PAD   :', call({'id':2,'cmd':'pad_status'}))
print('READ  :', call({'id':3,'cmd':'read_ram','addr':'0xBFC00000','len':16}))
print('WTRG  :', call({'id':4,'cmd':'wtrace_ranges'}))
print('FNARMS:', call({'id':5,'cmd':'fntrace_arms'}))
s.close()
