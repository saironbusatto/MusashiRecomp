"""Convert one or more BMPs (cmd-line args) into PNGs of the same name."""
from PIL import Image
import sys
for f in sys.argv[1:]:
    out = f.rsplit('.', 1)[0] + '.png'
    Image.open(f).convert('RGB').save(out)
    print(f"{f} -> {out}")
