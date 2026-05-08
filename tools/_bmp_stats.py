"""Sample BMP pixel stats to verify rendering without an image viewer."""
import struct, sys
path = sys.argv[1] if len(sys.argv) > 1 else 'psx_screenshot.bmp'
with open(path, 'rb') as f:
    f.seek(10); off = struct.unpack('<I', f.read(4))[0]
    f.seek(18); w, h = struct.unpack('<ii', f.read(8))
    print(f'Image: {w}x{h}, pixel data offset {off}')
    row_bytes = ((w * 3 + 3) // 4) * 4
    rows = abs(h)
    f.seek(off)
    rgb_rows = []
    for r in range(rows):
        rgb_rows.append(f.read(row_bytes))
    rgb_rows.reverse()  # BMP is bottom-up; reverse to make index 0 == top
    def sample(top, bot, label):
        total = 0; nonblack = 0
        red = green = blue = white = 0
        for r in range(top, bot):
            row = rgb_rows[r]
            for x in range(0, w * 3, 12):
                total += 1
                b, g, rr = row[x], row[x + 1], row[x + 2]
                if b > 30 or g > 30 or rr > 30:
                    nonblack += 1
                if rr > 100 and g < 50 and b < 50: red += 1
                if g > 100 and rr < 50 and b < 50: green += 1
                if b > 100 and rr < 50 and g < 50: blue += 1
                if b > 200 and g > 200 and rr > 200: white += 1
        pct = (100 * nonblack // total) if total else 0
        print(f'  {label} (rows {top}..{bot}): {nonblack}/{total} non-black ({pct}%) | R={red} G={green} B={blue} W={white}')
    sample(0, rows // 3, 'top')
    sample(rows // 3, 2 * rows // 3, 'middle')
    sample(2 * rows // 3, rows, 'bottom')
    sample(0, rows, 'whole')
