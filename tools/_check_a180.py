"""Read the modal-push gate and adjacent state."""
import socket, json, struct
s = socket.create_connection(('127.0.0.1', 4370), timeout=10)
def send(p):
    s.sendall((json.dumps(p) + '\n').encode())
    buf = b''
    while True:
        c = s.recv(1<<20)
        if not c: break
        buf += c
        if buf.strip().endswith(b'}'):
            try: return json.loads(buf.decode())
            except: continue

for (a, ln, lbl) in [
    (0x8007A180, 16, '_DAT_8007a180 (modal gate)'),
    (0x80066BB8, 16, 'cursor block (bb8/bbc/bc0/bc4)'),
    (0x8007A12C, 16, '_DAT_8007a12c..a134 (init counters)'),
    (0x80079E24, 4, '_DAT_80079e24 (sub-state idx)'),
    (0x80079ED0, 4, '_DAT_80079ed0 (-1 sentinel)'),
    (0x80066940, 32, 'state block'),
]:
    rr = send({'id':1,'cmd':'read_ram','addr':f'0x{a:08X}','len':ln})
    b = bytes.fromhex(rr.get('hex',''))
    print(f"{lbl}:")
    for i in range(0, len(b), 4):
        if i+4 > len(b): break
        w = struct.unpack('<I', b[i:i+4])[0]
        print(f"  0x{a+i:08X} = 0x{w:08X}  ({w})")
s.close()
