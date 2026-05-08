import re
text = open('generated/SCPH1001_full.c').read()
for m in re.finditer(r'psx_dispatch\(cpu,\s*0x([0-9A-Fa-f]+)u?\);', text):
    addr = int(m.group(1), 16)
    if (addr & 0x000FFFFF) == 0x428F0:
        print(f'0x{addr:08X}')
