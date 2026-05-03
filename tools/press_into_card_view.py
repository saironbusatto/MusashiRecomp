"""Press CROSS in Beetle to advance from MEMCARD selector to per-card view, then check state."""
import socket, json

def call(d):
    s = socket.create_connection(('127.0.0.1', 4370)); s.settimeout(60.0)
    s.sendall((json.dumps(d) + '\n').encode())
    buf = b''
    while True:
        try: c = s.recv(65536)
        except socket.timeout: break
        if not c: break
        buf += c
        depth = 0; in_str = False; esc = False
        for b in buf:
            if esc: esc = False; continue
            if b == 0x5C: esc = True; continue
            if b == 0x22: in_str = not in_str; continue
            if in_str: continue
            if b == 0x7B: depth += 1
            elif b == 0x7D: depth -= 1
        if depth == 0 and buf.strip(): break
    s.close()
    return json.loads(buf.decode())

print('initial substate:',
      call({'id':1,'cmd':'emu_read_ram','addr':'0x80066940','len':4}))

print('press CROSS 240 frames:',
      call({'id':2,'cmd':'emu_press','buttons':0xBFFF,'frames':240}))

print('post substate:',
      call({'id':3,'cmd':'emu_read_ram','addr':'0x80066940','len':4}))
print('post shell state:',
      call({'id':3,'cmd':'emu_read_ram','addr':'0x80066948','len':4}))

print('settle:', call({'id':4,'cmd':'emu_step','count':60}))
print('post-settle substate:',
      call({'id':5,'cmd':'emu_read_ram','addr':'0x80066940','len':4}))
print('post-settle shell state:',
      call({'id':6,'cmd':'emu_read_ram','addr':'0x80066948','len':4}))
