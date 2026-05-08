import re
text = open('generated/SCPH1001_full.c').read()
def norm(addr):
    phys = addr & 0x1FFFFFFF
    if 0x1FC10000 <= phys <= 0x1FC17FFF:
        phys = phys - 0x1FC10000 + 0x500
    if 0x30000 <= phys <= 0x5AFFF:
        phys = phys - 0x30000 + 0x1FC18000
    return phys

for m in re.finditer(r'psx_dispatch\(cpu,\s*0x([0-9A-Fa-f]+)u?\);', text):
    addr = int(m.group(1), 16)
    if norm(addr) == 0x1005A910:
        idx = m.start()
        # find function this call is in by walking back to prev `void func_`
        prev = text.rfind('void func_', 0, idx)
        line_start = text.rfind('\n', 0, idx) + 1
        line = text[line_start:text.find('\n', idx)]
        # find label above
        lbl_idx = text.rfind('label_BFC', 0, idx)
        lbl = text[lbl_idx:text.find(':', lbl_idx)+1] if lbl_idx > 0 else ''
        print(f"addr=0x{addr:08X} norm=0x{norm(addr):08X}")
        print(f"call line: {line}")
        print(f"nearest label: {lbl}")
        # print 8 lines before the call
        ctx_start = text.rfind('\n', 0, idx - 400) + 1
        ctx = text[ctx_start:text.find('\n', idx)]
        print("context:")
        print(ctx)
        break
