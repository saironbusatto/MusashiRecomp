"""Diff critical memory addresses between runtime (4470) and beetle (4380)
to find where Tomba's state machine diverges in OPTIONS-black."""
import sys, struct
sys.path.insert(0, 'F:/Projects/psxrecomp/psxrecomp/tools/_options_session')
import _dbg

def read_word(port, addr):
    try:
        return _dbg.read_word(port, addr)
    except Exception as e:
        return f"ERR: {e}"

def read_bytes(port, addr, n):
    try:
        r = _dbg.call(port, 'read_ram', addr=f'0x{addr:08X}', len=n, timeout=4)
        if not r.get('ok'): return f"ERR: {r}"
        return bytes.fromhex(r['hex'])
    except Exception as e:
        return f"ERR: {e}"

# Ping both
p_rt = _dbg.call(4470, "ping", timeout=2)
p_be = _dbg.call(4380, "ping", timeout=2)
print(f"runtime fr={p_rt.get('frame','?')} | beetle fr={p_be.get('frame','?')}")
print()

# Critical addresses to compare
print(f"{'addr':>12}  {'runtime':>12}  {'beetle':>12}  {'match':>6}")
addrs = [
    (0x1F8001D4, "scratchpad+0x1D4 (state-struct ptr)"),
    (0x80097520, "fn ptr table+0"),
    (0x80097524, "fn ptr table+4 (state-0 helper)"),
    (0x80097528, "fn ptr table+8"),
    (0x8009752C, "fn ptr table+C"),
    (0x8009B3A0, "state-0 helper return ptr (first word)"),
    (0x8009B3D4, "*** mem[ptr+0x34] high half check"),
    (0x8009EB58, "side-path read"),
]
for addr, label in addrs:
    rt = read_word(4470, addr)
    be = read_word(4380, addr)
    if isinstance(rt, int) and isinstance(be, int):
        m = "OK" if rt == be else "*DIFF*"
        print(f"  0x{addr:08X}  0x{rt:08X}  0x{be:08X}  {m}  {label}")
    else:
        print(f"  0x{addr:08X}  {rt}  {be}  ?  {label}")

# Compare the full state struct (256 bytes from runtime's ptr)
print()
print("=== Full state struct comparison ===")
rt_ptr = read_word(4470, 0x1F8001D4)
be_ptr = read_word(4380, 0x1F8001D4)
print(f"runtime ptr = 0x{rt_ptr:08X}, beetle ptr = 0x{be_ptr:08X}")
if rt_ptr == be_ptr and isinstance(rt_ptr, int):
    rt_buf = read_bytes(4470, rt_ptr, 256)
    be_buf = read_bytes(4380, be_ptr, 256)
    if isinstance(rt_buf, bytes) and isinstance(be_buf, bytes):
        for i in range(0, 256, 16):
            rt_row = rt_buf[i:i+16]
            be_row = be_buf[i:i+16]
            if rt_row != be_row:
                diff = ' '.join('!!' if rt_row[j] != be_row[j] else '..' for j in range(16))
                print(f"  +0x{i:03X}  RT: {' '.join(f'{x:02x}' for x in rt_row)}")
                print(f"         BE: {' '.join(f'{x:02x}' for x in be_row)}")
                print(f"         {diff}")
                print()

# Compare 0x8009B3A0 helper return struct too
print()
print("=== 0x8009B3A0 helper-return struct (256 bytes) ===")
rt_buf = read_bytes(4470, 0x8009B3A0, 256)
be_buf = read_bytes(4380, 0x8009B3A0, 256)
if isinstance(rt_buf, bytes) and isinstance(be_buf, bytes):
    any_diff = False
    for i in range(0, 256, 16):
        rt_row = rt_buf[i:i+16]
        be_row = be_buf[i:i+16]
        if rt_row != be_row:
            any_diff = True
            print(f"  +0x{i:03X}  RT: {' '.join(f'{x:02x}' for x in rt_row)}")
            print(f"         BE: {' '.join(f'{x:02x}' for x in be_row)}")
    if not any_diff:
        print("  (IDENTICAL)")
