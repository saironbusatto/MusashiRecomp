"""Dump state-6 entry (widget 0xA modal). Same layout as state-7.
+0x04 = widget id (should be 0xA)
+0x1C = SQUARE-action widget id
+0x20 = CIRCLE-action widget id
+0x24 = TRIANGLE-action widget id
+0x28 = CROSS-action widget id   ← YES action
+0x34 = D-pad handler PC
+0x38 = D-pad mask
"""
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
def to_int(v):
    if isinstance(v,str): return int(v,16) if v.startswith('0x') else int(v)
    return v

ENTRY = 0x801135F8  # state 6 (per earlier scan, widget 0xA)
LEN = 0x80
print(f"State-6 entry @ 0x{ENTRY:08X}:")
rr = send({'id':1,'cmd':'read_ram','addr':f'0x{ENTRY:08X}','len':LEN})
b = bytes.fromhex(rr.get('hex',''))
labels = {0x00:'header', 0x04:'widget_id', 0x1C:'SQUARE_widget',
          0x20:'CIRCLE_widget', 0x24:'TRIANGLE_widget',
          0x28:'CROSS_widget', 0x34:'dpad_handler_PC', 0x38:'dpad_mask'}
for i in range(0, len(b), 4):
    if i+4 > len(b): break
    w = struct.unpack('<I', b[i:i+4])[0]
    lbl = labels.get(i, '')
    print(f"  +0x{i:02X}: 0x{w:08X}  {lbl}")

# also dump live state right now
print("\nLive state:")
for (a, ln, lbl) in [(0x80066940, 16, 'state_block (substate, [+4], main)'),
                      (0x80066BB8, 16, 'cursor (bb8/bbc/bc0/bc4)'),
                      (0x80078320, 16, 'state_idx/widget'),
                      (0x8007A180, 4,  'modal gate')]:
    rr = send({'id':1,'cmd':'read_ram','addr':f'0x{a:08X}','len':ln})
    bb = bytes.fromhex(rr.get('hex',''))
    print(f"  {lbl}:")
    for i in range(0, len(bb), 4):
        if i+4 > len(bb): break
        w = struct.unpack('<I', bb[i:i+4])[0]
        print(f"    0x{a+i:08X} = 0x{w:08X}")
s.close()
