import re
with open('generated/SCPH1001_dispatch.c', 'r') as f:
    d = f.read()
in_table = set()
for m in re.finditer(r'\{ 0x([0-9A-Fa-f]{8})u', d):
    in_table.add(int(m.group(1), 16))
print('total entries:', len(in_table))
for t in ['0xBFC42990', '0xBFC429A0', '0xBFC42960', '0xBFC42BB0', '0xBFC42B00']:
    addr = int(t, 16)
    phys = addr & 0x1FFFFFFF
    print(f'{t} -> phys=0x{phys:08X} in_table={phys in in_table}')
