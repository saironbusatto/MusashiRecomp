import struct, sys
with open('F:/Projects/psxrecomp/psxrecomp/bios/SCPH1001.BIN', 'rb') as f:
    data = f.read()
# Read inputs as pc, length
start_pc = int(sys.argv[1], 16) if len(sys.argv) > 1 else 0xBFC16060
length   = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0x40
off0 = start_pc - 0xBFC00000
for i in range(0, length, 4):
    w = struct.unpack_from('<I', data, off0 + i)[0]
    pc = start_pc + i
    print(f'{pc:08x}: {w:08x}')
