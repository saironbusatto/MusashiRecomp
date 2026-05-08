"""Capture who calls FUN_bfc1e1cc (substate dispatcher) with a0=11.

Strategy:
1. Connect to psx-runtime debug server.
2. Set fn_filter to the dispatcher PC.  The recompiler's normalize_address
   masks with 0x1FFFFFFF, but the runtime may store either the raw RAM PC
   (0x800361CC) or the masked phys (0x000361CC).  We try a wide-ish range.
3. Clear fn_entry ring.
4. Drive input via debug commands: CROSS, DOWN, DOWN, CROSS — the standard
   path to navigate into MEMORY CARD → COPY ALL menu and confirm.
5. Dump fn_entry; print all hits with their ra (caller) and a0 (substate arg).

Run with psx-runtime already at the BIOS shell, MEMORY CARD reachable via
CROSS on the splash screen.
"""
import socket, json, sys, time, struct

PORT = 4370

def send(s, payload):
    s.sendall((json.dumps(payload) + "\n").encode())
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

def press_combo(s, mask, frames):
    """Hold a button mask for N frames. Mask format: bitwise inverted PSX pad word.
    Active-low pad: 0xFFFF=all-released. Bit 14=CROSS, bit 6=DOWN."""
    send(s, {"id": 1, "cmd": "set_input", "value": f"0x{mask & 0xFFFF:04X}"})
    time.sleep(frames / 60.0)

def clear_input(s):
    send(s, {"id": 1, "cmd": "clear_input"})

def hold(mask, frames):
    return (mask, frames)

s = socket.create_connection(("127.0.0.1", PORT), timeout=10)

# 1. Set filter — phys-ROM encoding (recompiler stores func_addr as phys = pc & 0x1FFFFFFF).
print("Setting fn_filter to phys-ROM 0x1FC1E1CC..0x1FC1E1D0 ...")
r = send(s, {"id": 1, "cmd": "fn_filter", "lo": "0x1FC1E1CC", "hi": "0x1FC1E1D0"})
print(f"  -> {r}")

# Also try wider: include kseg0/kseg1 raw addresses by overlapping.
# We only get ONE filter range, so pick the most likely (RAM phys).

# 2. Clear ring.
print("Clearing fn_entry ring ...")
print(f"  -> {send(s, {'id': 1, 'cmd': 'fn_clear'})}")

# 3. Make sure we're not paused.
send(s, {"id": 1, "cmd": "continue"})

# 4. Drive input. Pad bits are ACTIVE-LOW: 0=pressed.
PAD_DOWN  = 1 << 6
PAD_CROSS = 1 << 14

ALL_RELEASED = 0xFFFF
def hold_press(bit, ms_pressed=120, ms_released=120):
    mask = ALL_RELEASED & ~bit
    send(s, {"id": 1, "cmd": "set_input", "value": f"0x{mask:04X}"})
    time.sleep(ms_pressed / 1000.0)
    send(s, {"id": 1, "cmd": "set_input", "value": f"0x{ALL_RELEASED:04X}"})
    time.sleep(ms_released / 1000.0)

print("Driving sequence: CROSS, DOWN, DOWN, CROSS ...")
hold_press(PAD_CROSS, 200, 250)   # enter MEMORY CARD
hold_press(PAD_DOWN,  150, 200)
hold_press(PAD_DOWN,  150, 200)
hold_press(PAD_CROSS, 200, 500)   # confirm COPY ALL

# Release to neutral.
send(s, {"id": 1, "cmd": "clear_input"})
time.sleep(0.3)

# 5. Dump the ring.
print("Dumping fn_entry hits within filter range ...")
r = send(s, {"id": 1, "cmd": "fn_entry_dump",
              "addr_lo": "0x1FC1E1CC", "addr_hi": "0x1FC1E1D0",
              "seq_lo": "0", "count": 1000})
total   = r.get('total', 0)
oldest  = r.get('oldest', 0)
entries = r.get('entries', [])
print(f"  total={total} oldest={oldest} returned={len(entries)}")

# Also print live state.
state_block = send(s, {"id": 1, "cmd": "read_ram", "addr": "0x80066940", "len": 16})
hx = state_block.get('hex', '')
if hx:
    bb = bytes.fromhex(hx)
    sub  = struct.unpack("<I", bb[0:4])[0]
    flag = struct.unpack("<I", bb[4:8])[0]
    main = struct.unpack("<I", bb[8:12])[0]
    print(f"\n  live state: substate=0x{sub:X}  +0x6944=0x{flag:X}  main=0x{main:X}")

print("\nAll fn_entry hits in the dispatcher region (sorted by seq):")
for e in entries:
    print(f"  seq={e['seq']:>6} func={e['func']} ra={e['ra']} a0={e['a0']} a1={e.get('a1','?')} frame={e['frame']}")

# Highlight a0=11 hits.
hot = [e for e in entries if int(e['a0'], 16) == 0xB]
print(f"\n=== {len(hot)} hits where a0=0xB (the bad caller path):")
for e in hot:
    print(f"  seq={e['seq']:>6} ra={e['ra']} (caller PC) func={e['func']} a0=0xB a1={e.get('a1','?')} frame={e['frame']}")

# If empty, try fallback filter ranges and report.
if not entries:
    print("\nNo hits in 0x000361CC range. Trying alternative encodings...")
    for (lo, hi, label) in [
        ("0x800361CC", "0x800361D0", "raw KUSEG"),
        ("0x1FC1E1CC", "0x1FC1E1D0", "phys-ROM"),
        ("0xBFC1E1CC", "0xBFC1E1D0", "raw KSEG1 ROM"),
    ]:
        r = send(s, {"id": 1, "cmd": "fn_filter", "lo": lo, "hi": hi})
        print(f"  Filter set to {label} {lo}..{hi}")
        # Replay the press of CROSS to retrigger.
        hold_press(PAD_CROSS, 200, 500)
        send(s, {"id": 1, "cmd": "clear_input"})
        time.sleep(0.3)
        r = send(s, {"id": 1, "cmd": "fn_entry_dump",
                      "addr_lo": lo, "addr_hi": hi,
                      "seq_lo": "0", "count": 50})
        n = len(r.get('entries', []))
        print(f"    -> {n} entries")
        if n:
            for e in r['entries'][:20]:
                print(f"      seq={e['seq']} func={e['func']} ra={e['ra']} a0={e['a0']}")
            break

s.close()
