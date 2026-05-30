import sys
sys.path.insert(0, 'F:/Projects/psxrecomp/psxrecomp/tools/_options_session')
import _dbg
addr = int(sys.argv[1], 16)
size = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0x200
r = _dbg.call(4470, 'read_ram', addr=f'0x{addr:08X}', len=size, timeout=4)
b = bytes.fromhex(r['hex'])
print(f"Contents of 0x{addr:08X} ({size} bytes):")
for i in range(0, len(b), 32):
    line = ' '.join(f'{x:02x}' for x in b[i:i+32])
    non_zero = sum(1 for x in b[i:i+32] if x != 0)
    marker = "" if non_zero == 0 else f" ({non_zero}/32 non-zero)"
    print(f"  +0x{i:03X}: {line}{marker}")
