"""Re-run dispatch_check with ROM-physical addresses (subtract 0x80018000
from Beetle RAM PCs to get recomp dispatch target)."""
import socket, json
def call(payload):
    s = socket.create_connection(('127.0.0.1', 4370), timeout=10)
    s.sendall((json.dumps(dict(payload, id=1))+'\n').encode())
    buf = b''
    while True:
        c = s.recv(1<<20)
        if not c: break
        buf += c
        try: return json.loads(buf.decode())
        except: pass
    raise RuntimeError("incomplete")

# Shell relocation: RAM 0x80030000 = ROM 0x1FC18000
# Verified: RAM 0x800394B0 = ROM 0x1FC214B0
SHELL_RAM_BASE = 0x80030000
SHELL_ROM_BASE = 0x1FC18000
def ram_to_rom(ram):
    if ram >= SHELL_RAM_BASE:
        return (ram - SHELL_RAM_BASE) + SHELL_ROM_BASE
    return ram  # non-shell, leave unchanged

CANDIDATES = [
    (0x8003A700, 'main_state=4 entry candidate'),
    (0x8003A710, 'main_state=4 writer PC (inside func)'),
    (0x8003A6F8, 'main_state=4 writer-2-instructions earlier'),
    (0x80046470, 'SetStateForWidget alt entry'),
    (0x80046468, 'SetStateForWidget body'),
    (0x8004644C, 'SetStateForWidget candidate'),
    (0x80039840, 'coordinator dispatch table internal'),
    (0x800394B0, 'coordinator entry (CONFIRMED RAM=ROM map)'),
    (0x800321BC, 'modal stub'),
    (0x800397B0, 'case-A coordinator close'),
    (0x8003C640, 'cursor_mirror2 writer 1'),
    (0x80032300, 'cursor_mirror2 writer 2'),
    (0x80032164, 'caller of cursor_mirror2 writer 1 (Beetle)'),
    (0x80032174, 'caller of cursor_mirror2 writer 2 (Beetle)'),
    (0x800465E0, 'caller of SetStateForWidget body (Beetle)'),
    (0x80008A58, 'coord caller A'),
    (0x80030558, 'coord caller B'),
    (0x80031660, 'lookup caller'),
    (0x80030534, 'coord parent'),
    (0x80046664, 'lookup caller 2'),
    (0x8003ACE4, 'cursor_mirror2 writer (group B PC)'),
]

print(f"=== Recomp dispatch_check — ROM-physical addresses ===")
print(f"{'RAM PC':<12} {'ROM PC':<12} {'found':<6} label")
for ram, lbl in CANDIDATES:
    rom = ram_to_rom(ram)
    r = call({'cmd':'dispatch_check','addr':f'0x{rom:08X}'})
    found = r.get('found')
    print(f"0x{ram:08X}  0x{rom:08X}  {str(found):<6} {lbl}")
