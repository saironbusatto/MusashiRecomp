"""Find where 'EC' bytes (0x45, 0x43) appear in runtime memory but not beetle.
That tells us if 0x43450000 at 0x8009B3D4 is from a stale buffer/string."""
import sys
sys.path.insert(0, 'F:/Projects/psxrecomp/psxrecomp/tools/_options_session')
import _dbg

# Scan a wide range of low RAM (0x80010000..0x80100000) for any place where
# runtime has bytes "45 43" but beetle doesn't.
# Too wide; let's narrow to 0x8009B000..0x8009B600 (around the diverging struct).

ranges = [
    (0x80097000, 0x800A0000, "fn ptr table region + 0x8009B... region"),
    (0x801FD000, 0x801FE000, "TCB region"),
]

for lo, hi, label in ranges:
    print(f"=== {label}: 0x{lo:08X}..0x{hi:08X} ===")
    chunk = 0x800
    for base in range(lo, hi, chunk):
        sz = min(chunk, hi - base)
        try:
            r_rt = _dbg.call(4470, 'read_ram', addr=f'0x{base:08X}', len=sz, timeout=4)
            r_be = _dbg.call(4380, 'read_ram', addr=f'0x{base:08X}', len=sz, timeout=4)
            if not r_rt.get('ok') or not r_be.get('ok'): continue
            bt_rt = bytes.fromhex(r_rt['hex'])
            bt_be = bytes.fromhex(r_be['hex'])
            for i in range(len(bt_rt) - 1):
                if bt_rt[i] == 0x45 and bt_rt[i+1] == 0x43:
                    if bt_be[i] != 0x45 or bt_be[i+1] != 0x43:
                        # runtime has "EC", beetle doesn't
                        addr = base + i
                        ctx_lo = max(0, i-4)
                        ctx_hi = min(len(bt_rt), i+8)
                        rt_ctx = ' '.join(f'{x:02x}' for x in bt_rt[ctx_lo:ctx_hi])
                        be_ctx = ' '.join(f'{x:02x}' for x in bt_be[ctx_lo:ctx_hi])
                        print(f"  0x{addr:08X}  RT: {rt_ctx}")
                        print(f"              BE: {be_ctx}")
        except Exception as e:
            print(f"  err at 0x{base:08X}: {e}")
            break
