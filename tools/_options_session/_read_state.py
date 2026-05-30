import sys, struct
sys.path.insert(0, 'F:/Projects/psxrecomp/psxrecomp/tools/_options_session')
import _dbg

# Read scratchpad ptr at 0x1F8001D4
ptr_word = _dbg.read_word(4470, 0x1F8001D4)
print(f"scratchpad[0x1D4] = ptr 0x{ptr_word:08X}")

# Read mem[ptr + 0x4E] (halfword), and surrounding bytes for context
r = _dbg.call(4470, 'read_ram', addr=f'0x{ptr_word:08X}', len=0x80, timeout=3)
b = bytes.fromhex(r['hex'])
print(f"\nContents of mem[{ptr_word:08X}..]:")
for i in range(0, len(b), 16):
    line = ' '.join(f'{x:02x}' for x in b[i:i+16])
    print(f"  +{i:02X}: {line}")

state = struct.unpack_from('<H', b, 0x4E)[0]
print(f"\nstate_index = mem[ptr+0x4E] (halfword) = 0x{state:04X} ({state} decimal)")
if state < 6:
    print(f"  -> would dispatch via jump table 0x800E738C[{state}]")
else:
    print(f"  -> state >= 6, chain handler SKIPS (this is the 'do nothing' branch)")

# Read the jump table itself to see what each state would do
jt = _dbg.call(4470, 'read_ram', addr='0x800E738C', len=6*4, timeout=3)
jtb = bytes.fromhex(jt['hex'])
print(f"\nJump table 0x800E738C (6 entries):")
for i in range(6):
    tgt = struct.unpack_from('<I', jtb, i*4)[0]
    print(f"  state={i}: target=0x{tgt:08X}")
