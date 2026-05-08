"""Read modal state and slot status during the YES/NO inert state."""
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

print("=== mc_status ===")
print(json.dumps(send({'id':1,'cmd':'mc_status'}), indent=2))

print("\n=== sio_state ===")
print(json.dumps(send({'id':1,'cmd':'sio_state'}), indent=2))

print("\n=== freeze_check (window=64) ===")
fc = send({'id':1,'cmd':'freeze_check','window':64})
print(f"  current_func: {fc.get('current_func')}")
print(f"  last_store_pc: {fc.get('last_store_pc')}")
print(f"  hist top: {fc.get('hist',[])[:5]}")

print("\n=== live state ===")
import struct
for (a, ln, lbl) in [
    (0x80066940, 16, 'state_block'),
    (0x80066BB8, 16, 'cursor (bb8/bbc/bc0/bc4)'),
    (0x80078320, 16, 'state_idx/widget'),
    (0x8007A180, 4,  'modal gate'),
    (0x80087144, 16, 'slot status (0x8008 -7EBC)?'),  # guess; may not be
]:
    rr = send({'id':1,'cmd':'read_ram','addr':f'0x{a:08X}','len':ln})
    bb = bytes.fromhex(rr.get('hex',''))
    print(f"  {lbl}:")
    for i in range(0, len(bb), 4):
        if i+4 > len(bb): break
        w = struct.unpack('<I', bb[i:i+4])[0]
        print(f"    0x{a+i:08X} = 0x{w:08X}")

s.close()
