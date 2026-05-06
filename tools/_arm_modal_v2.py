"""Arm both backends for the modal investigation, with the bumped wtrace cap.

Recomp wtrace_add cap is now 64. We add full modal-cell coverage:
  cursor + cursor_mirror2 + main_state          (0x66BB8..0x66BD0)
  state_idx + widget + cursor_anim              (0x78318..0x78330)
  cursor_mirror                                  (0x79F60..0x79F70)

Beetle wtrace_arm: same 3 ranges.
Beetle fntrace_arm: 9 candidate targets near Beetle's known press chain.
"""
import socket, json
def call(port, payload):
    s = socket.create_connection(('127.0.0.1', port), timeout=5)
    s.sendall((json.dumps(dict(payload, id=1))+'\n').encode())
    buf = b''
    while True:
        c = s.recv(1<<16)
        if not c: break
        buf += c
        if buf.endswith(b'\n'): break
    s.close()
    return json.loads(buf.decode())

# ---- Beetle ----
print("=== Beetle (4380) wtrace ===")
for lo, hi, lbl in [
    (0x80066BB8, 0x80066BD0, 'cursor + cursor_mirror2 + main_state'),
    (0x80078318, 0x80078330, 'state_idx + widget + cursor_anim'),
    (0x80079F60, 0x80079F70, 'cursor_mirror'),
]:
    r = call(4380, {'cmd':'wtrace_arm', 'lo':f'0x{lo:08X}', 'hi':f'0x{hi:08X}'})
    print(f'  arm [{lo:08X}..{hi:08X}] ({lbl}): ok={r.get("ok")}')

print("\n=== Beetle (4380) fntrace ===")
for tgt, lbl in [
    (0x8004644C, 'SetStateForWidget candidate (RAM)'),
    (0x80046468, 'SetStateForWidget body / RA seen in wtrace'),
    (0x80046470, 'SetStateForWidget alt entry'),
    (0x8003C640, 'cursor_mirror2 writer 1 entry candidate'),
    (0x80032300, 'cursor_mirror2 writer 2 entry candidate'),
    (0x800394B0, 'coordinator (control)'),
    (0x800321BC, 'modal stub'),
    (0x80008A58, 'coord caller A'),
    (0x80030558, 'coord caller B'),
    (0x8003A700, 'main_state=4 entry'),
    (0x8003A710, 'main_state=4 writer PC'),
]:
    r = call(4380, {'cmd':'fntrace_arm', 'target':f'0x{tgt:08X}'})
    print(f'  arm 0x{tgt:08X} ({lbl}): ok={r.get("ok")} slot={r.get("slot","?")}')

# ---- Recomp ----
print("\n=== Recomp (4370) wtrace (cap is now 64) ===")
for lo, hi, lbl in [
    (0x00066BB8, 0x00066BC0, 'cursor + cursor_mirror2 (main_state already in slot 14)'),
    (0x00078318, 0x00078330, 'state_idx + widget + cursor_anim'),
    (0x00079F60, 0x00079F70, 'cursor_mirror'),
]:
    r = call(4370, {'cmd':'wtrace_add', 'lo':f'0x{lo:08X}', 'hi':f'0x{hi:08X}'})
    print(f'  add [{lo:08X}..{hi:08X}] ({lbl}): ok={r.get("ok")} slot={r.get("slot","?")}')

print("\n=== Recomp wtrace_ranges ===")
r = call(4370, {'cmd':'wtrace_ranges'})
print(f"count={r.get('count')}")
for rng in r.get('ranges', []):
    print(f"  slot {rng['slot']:>2}: {rng['lo']}..{rng['hi']}")

print("\n=== Beetle wtrace_ranges ===")
r = call(4380, {'cmd':'wtrace_ranges'})
print(f"count={r.get('count')}")
for rng in r.get('ranges', []):
    print(f"  slot {rng['slot']:>2}: {rng['lo']}..{rng['hi']}")
