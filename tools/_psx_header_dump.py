import struct, os, sys
p = sys.argv[1] if len(sys.argv) > 1 else 'F:/Projects/TombaRecomp/tomba/SCUS_942.36'
with open(p, 'rb') as f:
    h = f.read(0x800)
magic = h[:8]
init_pc, init_gp = struct.unpack('<II', h[0x10:0x18])
t_addr, t_size = struct.unpack('<II', h[0x18:0x20])
d_addr, d_size = struct.unpack('<II', h[0x20:0x28])
b_addr, b_size = struct.unpack('<II', h[0x28:0x30])
sp_base, sp_off  = struct.unpack('<II', h[0x30:0x38])
print('magic   :', magic)
print(f'init_pc : 0x{init_pc:08X}')
print(f'init_gp : 0x{init_gp:08X}')
print(f't_addr  : 0x{t_addr:08X}')
print(f't_size  : 0x{t_size:08X}')
print(f'd_addr  : 0x{d_addr:08X}  d_size: 0x{d_size:08X}')
print(f'b_addr  : 0x{b_addr:08X}  b_size: 0x{b_size:08X}')
print(f'sp_base : 0x{sp_base:08X}  sp_off: 0x{sp_off:08X}')
print()
sz = os.path.getsize(p)
nh = p + '_no_header'
if os.path.exists(nh):
    sz_nh = os.path.getsize(nh)
    print(f'headered size : {sz}')
    print(f'no_header size: {sz_nh}  (delta = {sz - sz_nh} = 0x{sz-sz_nh:X})')
