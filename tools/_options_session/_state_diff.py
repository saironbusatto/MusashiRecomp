import sys
sys.path.insert(0, 'F:/Projects/psxrecomp/psxrecomp/tools/_options_session')
import _dbg

print(f"{'thing':>40}  {'runtime':>12}  {'beetle':>12}")
for label, addr, width in [
    ("scratchpad+0x1D4 (ptr)",      0x1F8001D4, 'w'),
    ("ptr+0x4C (full word w/ state)", 0,        'special_state'),
    ("0x8009B3A0+0x34",              0x8009B3D4, 'w'),
    ("0x8009B3A0+0x49 (byte at 0x49)", 0x8009B3E9, 'b'),
    ("0x8009EB58",                   0x8009EB58, 'w'),
    ("frame count",                  -1,         'frame'),
]:
    if width == 'frame':
        rt = _dbg.call(4470, "ping", timeout=2).get('frame','?')
        be = _dbg.call(4380, "ping", timeout=2).get('frame','?')
        print(f"  {label:>40}  {rt:>12}  {be:>12}")
        continue
    if width == 'special_state':
        # state is halfword at ptr+0x4E
        rt_ptr = _dbg.read_word(4470, 0x1F8001D4)
        be_ptr = _dbg.read_word(4380, 0x1F8001D4)
        rt_state_word = _dbg.read_word(4470, rt_ptr + 0x4C)
        be_state_word = _dbg.read_word(4380, be_ptr + 0x4C)
        rt_state = (rt_state_word >> 16) & 0xFFFF
        be_state = (be_state_word >> 16) & 0xFFFF
        print(f"  {'state @ ptr+0x4E':>40}  {rt_state:>12}  {be_state:>12}")
        continue
    if width == 'w':
        rt = _dbg.read_word(4470, addr)
        be = _dbg.read_word(4380, addr)
        mark = "" if rt == be else "  *DIFF*"
        print(f"  {label:>40}  0x{rt:08X}  0x{be:08X}{mark}")
    elif width == 'b':
        rt = _dbg.read_word(4470, addr & ~3)
        be = _dbg.read_word(4380, addr & ~3)
        byte_off = addr & 3
        rt_byte = (rt >> (byte_off * 8)) & 0xFF
        be_byte = (be >> (byte_off * 8)) & 0xFF
        mark = "" if rt_byte == be_byte else "  *DIFF*"
        print(f"  {label:>40}  0x{rt_byte:02X}{'         '}  0x{be_byte:02X}{'         '}{mark}")
