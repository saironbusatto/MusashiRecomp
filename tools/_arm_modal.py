"""Arm wtrace ranges + Beetle fntrace targets on both backends.

Beetle (4380) commands: wtrace_arm, fntrace_arm, wtrace_ranges, fntrace_arms
Recomp (4370) commands: wtrace_add, wtrace_ranges (different names — pre-existing)

Modal cells:
  0x80066BB8 cursor
  0x80066BBC cursor_mirror2
  0x80066BC0 main_state
  0x80078320 state_idx
  0x80078324 widget
  0x80078328 cursor_anim
  0x80079F64 cursor_mirror

These rings are always-on once armed. They stay armed until the process
exits. We don't reset before the press; we just look at the tail after.
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

# ---- Beetle wtrace ranges ----
print("=== Beetle (4380) wtrace ===")
for lo, hi, lbl in [
    (0x80066BB8, 0x80066BD0, 'cursor + cursor_mirror2 + main_state'),
    (0x80078318, 0x80078330, 'state_idx + widget + cursor_anim'),
    (0x80079F60, 0x80079F70, 'cursor_mirror'),
]:
    r = call(4380, {'cmd':'wtrace_arm', 'lo':f'0x{lo:08X}', 'hi':f'0x{hi:08X}'})
    print(f'  arm [{lo:08X}..{hi:08X}] ({lbl}): ok={r.get("ok")}')

# ---- Beetle fntrace targets ----
print("\n=== Beetle (4380) fntrace ===")
for tgt, lbl in [
    (0x8004644C, 'SetStateForWidget candidate (RAM)'),
    (0x80046468, 'SetStateForWidget body / RA seen in wtrace'),
    (0x80046470, 'SetStateForWidget alt entry'),
    (0x8003C640, 'cursor_mirror2 writer 1 entry candidate'),
    (0x80032300, 'cursor_mirror2 writer 2 entry candidate'),
    (0x800394B0, 'coordinator (control: confirmed dormant)'),
    (0x800321BC, 'modal stub'),
    (0x80008A58, 'coord caller A'),
    (0x80030558, 'coord caller B'),
]:
    r = call(4380, {'cmd':'fntrace_arm', 'target':f'0x{tgt:08X}'})
    print(f'  arm 0x{tgt:08X} ({lbl}): ok={r.get("ok")} slot={r.get("slot","?")}')

# ---- Recomp wtrace adds ----
print("\n=== Recomp (4370) wtrace ===")
for lo, hi, lbl in [
    (0x00066BB8, 0x00066BC0, 'cursor + cursor_mirror2 (main_state already armed)'),
    (0x00078318, 0x00078330, 'state_idx + widget + cursor_anim'),
    (0x00079F60, 0x00079F70, 'cursor_mirror'),
]:
    r = call(4370, {'cmd':'wtrace_add', 'lo':f'0x{lo:08X}', 'hi':f'0x{hi:08X}'})
    print(f'  add [{lo:08X}..{hi:08X}] ({lbl}): ok={r.get("ok")} slot={r.get("slot","?")}')

# Confirm
print("\n=== Beetle wtrace_ranges ===")
print(call(4380, {'cmd':'wtrace_ranges'}))
print("\n=== Beetle fntrace_arms ===")
print(call(4380, {'cmd':'fntrace_arms'}))
print("\n=== Recomp wtrace_ranges ===")
print(call(4370, {'cmd':'wtrace_ranges'}))
