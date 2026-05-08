"""Read the substate dispatch table at RAM 0x8005AC84 and identify which
index dispatches to the case that writes widget=0xB to main state mem[0x80066948].

Connects to local psx-runtime debug server (port 4370) and uses read_ram.

The dispatcher is at PC 0xBFC1E1CC; bounds check accepts a0 in [2..0x19].
Case body that writes 0xB is at PC 0xBFC1E2C0..0xBFC1E2CC.

PC values in the table may be ROM (0xBFC1E2C0) or RAM-shadowed (0x800362C0
or similar). We don't assume — we just print all 24 entries.
"""
import socket, json, struct, sys

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 4370
TABLE_BASE = 0x8005AC84
NUM_ENTRIES = 24

def read_ram(s, addr, length):
    s.sendall((json.dumps({"id": 1, "cmd": "read_ram", "addr": f"0x{addr:08X}", "len": length}) + "\n").encode())
    buf = b""
    while True:
        c = s.recv(65536)
        if not c: break
        buf += c
        if buf.strip().endswith(b'}'):
            try:
                return json.loads(buf.decode())
            except Exception:
                continue
    return json.loads(buf.decode())

s = socket.create_connection(("127.0.0.1", PORT), timeout=10)

# Read the jump table.
r = read_ram(s, TABLE_BASE, NUM_ENTRIES * 4)
hx = r.get('hex', '')
b = bytes.fromhex(hx) if hx else b''
print(f"# Substate dispatch table @ 0x{TABLE_BASE:08X} ({NUM_ENTRIES} entries)")
print(f"# raw len = {len(b)} bytes\n")

target_b_writer = None
for i in range(NUM_ENTRIES):
    if i*4+4 > len(b):
        break
    pc = struct.unpack("<I", b[i*4:i*4+4])[0]
    a0 = i + 2  # bounds check is (a0-2) < 0x18
    note = ""
    # Check if this PC corresponds to 0xBFC1E2C0 region (writes 0xB).
    # Could be ROM PC or RAM-shadow. Compute multiple aliases:
    #   ROM kseg1: 0xBFC1E2C0
    #   ROM kseg0: 0x9FC1E2C0
    #   RAM shadow: 0x800362C0  (0x80030000 + 0x62C0 if shell relocated)
    if pc in (0xBFC1E2C0, 0x9FC1E2C0, 0x800362C0):
        note = "  ← writes 0xB to mem[0x80066948]"
        target_b_writer = a0
    elif pc in (0xBFC1E260, 0x9FC1E260, 0x80036260):
        note = "  (writes 0x3a to state+substate)"
    elif pc in (0xBFC1E2A0, 0x9FC1E2A0, 0x800362A0):
        note = "  (writes 0x3b to substate)"
    print(f"  [a0={a0:>3}] (idx {i:>2}) -> 0x{pc:08X}{note}")

# Also dump current state for context.
print()
state_block = read_ram(s, 0x80066940, 16)
hx = state_block.get('hex', '')
if hx:
    bb = bytes.fromhex(hx)
    sub  = struct.unpack("<I", bb[0:4])[0]
    flag = struct.unpack("<I", bb[4:8])[0]
    main = struct.unpack("<I", bb[8:12])[0]
    aux  = struct.unpack("<I", bb[12:16])[0]
    print(f"# live state:")
    print(f"  mem[0x80066940] (substate)   = 0x{sub:08X}")
    print(f"  mem[0x80066944]              = 0x{flag:08X}")
    print(f"  mem[0x80066948] (main state) = 0x{main:08X}")
    print(f"  mem[0x8006694C]              = 0x{aux:08X}")

# Also read the staging state used by FUN_bfc1ffa8 / b0037FA8
r2 = read_ram(s, 0x80079E4C, 4)
hx2 = r2.get('hex', '')
if hx2:
    bb2 = bytes.fromhex(hx2)
    stg = struct.unpack("<I", bb2[0:4])[0]
    print(f"  mem[0x80079E4C] (staging)    = 0x{stg:08X}")

if target_b_writer is not None:
    print()
    print(f"# RESULT: substate value a0 = {target_b_writer} (0x{target_b_writer:X}) dispatches to the 0xB-writer case.")
    print(f"# So FUN_bfc1e1cc({target_b_writer}) ⇒ mem[0x80066948] = 0xB.")
    print(f"# Next: find callers passing a0={target_b_writer} to the dispatcher.")
else:
    print()
    print(f"# RESULT: no entry matched the 0xB-writer PC. Print the table values and adjust aliases.")

s.close()
