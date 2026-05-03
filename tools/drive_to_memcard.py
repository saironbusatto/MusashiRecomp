"""Drive Beetle from boot to memcard screen, then dump fn_trace callers."""
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

def read_byte(addr):
    r = call({'id': 9, 'cmd': 'emu_read_ram', 'addr': f'0x{addr:08X}', 'len': 1})
    return r.get('hex', '?')

print('--- pre-state ---')
print(f"frame: {call({'id':0,'cmd':'ping'}).get('frame')}")
print(f"shell state [0x66948] = {read_byte(0x80066948)}")

print()
print('--- run 240 frames idle (let shell settle to selector) ---')
print(call({'id':1,'cmd':'emu_step','count':240}))

print()
print(f"frame: {call({'id':0,'cmd':'ping'}).get('frame')}")
print(f"shell state [0x66948] = {read_byte(0x80066948)}")

print()
print('--- press CROSS for 240 frames ---')
print(call({'id':2,'cmd':'emu_press','buttons':0xBFFF,'frames':240}))

print()
print(f"frame: {call({'id':0,'cmd':'ping'}).get('frame')}")
print(f"shell state [0x66948] = {read_byte(0x80066948)}")

print()
print('--- settle 60 frames idle ---')
print(call({'id':3,'cmd':'emu_step','count':60}))

print()
print(f"frame: {call({'id':0,'cmd':'ping'}).get('frame')}")
print(f"shell state [0x66948] = {read_byte(0x80066948)}")
