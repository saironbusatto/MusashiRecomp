"""Add wtrace on the button-state edge table at 0x80066B68 + clear rings.
Then user presses CROSS once and we capture writers."""
import socket, json
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

# Existing ranges already cover 0x66BB8-0x66BD0 (cursor block).
# Add 0x66B68-0x66BB8 to cover the button-state edge table.
print("add button table:", send({'id':1,'cmd':'wtrace_add',
    'lo':'0x80066B68','hi':'0x80066BB8'}))

# Also widen fn_entry filter to whole BIOS for safety.
print("fn_filter:", send({'id':1,'cmd':'fn_filter',
    'lo':'0x1FC00000','hi':'0x1FC50000'}))

# Read current state
import struct
print("\n=== current button-state table @ 0x80066B68..BB8 ===")
rr = send({'id':1,'cmd':'read_ram','addr':'0x80066B68','len':0x50})
b = bytes.fromhex(rr.get('hex',''))
for i in range(0, len(b), 4):
    if i+4 > len(b): break
    w = struct.unpack('<I', b[i:i+4])[0]
    if w != 0:
        print(f"  0x{0x80066B68+i:08X} = 0x{w:08X}")
nz = sum(1 for i in range(0,len(b),4) if i+4<=len(b) and struct.unpack('<I',b[i:i+4])[0] != 0)
print(f"  ({nz} non-zero entries)")

# Clear rings
print("\n=== clearing rings ===")
print("fn_clear:", send({'id':1,'cmd':'fn_clear'}))
print("wtrace_clear:", send({'id':1,'cmd':'wtrace_clear'}))
print("\n>>> READY — press CROSS once in the modal, then run _capture_button_press.py")
s.close()
