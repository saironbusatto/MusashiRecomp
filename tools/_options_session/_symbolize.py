"""Map captured stack offsets to nearest preceding symbol from nm output."""
import subprocess, sys

# captured stack-frame offsets (relative to runtime image base)
offsets = [
    ("[ 2] SleepEx caller",        0x000B2219),
    ("[ 3]",                       0x000B4E51),
    ("[ 4]",                       0x000BE7F7),
    ("[ 5]",                       0x00460B2D),
    ("[ 6]",                       0x0063ECC7),
    ("[ 7]",                       0x006B12B0),
    ("[ 8]",                       0x000D889C),
    ("[ 9]",                       0x0045FCF0),
    ("[10] dispatcher",            0x0045FDC9),
    ("[11]",                       0x00011BC0A & 0xFFFFFF),  # 0x11BC0A is unusual
    ("[12] dispatcher",            0x0045FC97),
    ("[14] dispatcher",            0x0045FDC9),
    ("[15]",                       0x001A37AB),
    ("[16] dispatcher",            0x0045FC97),
    ("[18]",                       0x00142516),
    ("[20]",                       0x0013D8AC),
    ("[23]",                       0x00144B00),
    ("[26]",                       0x000B2B77),
    ("[27]",                       0x006BB07C),
    ("[28]",                       0x000B10C9),
    ("[29] start",                 0x000B1416),
]

# Load nm output
nm = subprocess.run(
    ['nm', 'F:/Projects/TombaRecomp/build/psx-runtime.exe'],
    capture_output=True, text=True
).stdout

# Build sorted list of (addr, name) for text symbols
syms = []
for line in nm.splitlines():
    parts = line.split()
    if len(parts) >= 3 and parts[1] in ('t','T','w','W'):
        try:
            a = int(parts[0], 16)
        except:
            continue
        # filter to image-base range (0x140000000+ standard mingw)
        if 0x140000000 <= a < 0x150000000:
            syms.append((a - 0x140000000, ' '.join(parts[2:])))
syms.sort()
print(f"Loaded {len(syms)} text symbols")

import bisect
addrs = [a for a,_ in syms]
for label, off in offsets:
    i = bisect.bisect_right(addrs, off) - 1
    if i < 0:
        print(f"  {label}: offset 0x{off:08X} -> (before all symbols)")
        continue
    sym_off, sym_name = syms[i]
    disp = off - sym_off
    # only show if displacement is reasonable (< 0x10000 = 64KB)
    note = "" if disp < 0x10000 else " (LARGE displacement — probably between symbols)"
    print(f"  {label}: offset 0x{off:08X} -> {sym_name} + 0x{disp:X}{note}")
