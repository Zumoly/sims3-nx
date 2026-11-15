"""Turn the image dumps from the console into a contact sheet.
usage: python rawview.py <capture_dir> <out.png>
Dumps are named dump<run>_<label>_<width>x<height>[_f].raw; _f marks 32-bit float depth."""
import glob, math, os, re, sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "work", "pydeps"))
import numpy as np
from PIL import Image, ImageDraw

paths = sorted(glob.glob(os.path.join(sys.argv[1], "dump*.raw")))
cells = []
for path in paths:
    name = os.path.basename(path)
    m = re.search(r"_(\d+)x(\d+)(_f)?\.raw$", name)
    w, h = int(m[1]), int(m[2])
    data = open(path, "rb").read()
    if len(data) < w * h * 4:
        print("%-44s truncated, %d bytes" % (name, len(data)))
        continue
    if m[3]:
        a = np.frombuffer(data, "<f4", w * h).reshape(h, w)
        lo, hi = float(a.min()), float(a.max())
        print("%-44s depth min %.4f max %.4f" % (name, lo, hi))
        g = ((a - lo) / max(hi - lo, 1e-6) * 255).astype(np.uint8)
        rgb = np.dstack([g, g, g])
    else:
        a = np.frombuffer(data, np.uint8, w * h * 4).reshape(h, w, 4)
        rgb = a[:, :, :3]
        print("%-44s mean rgb %.1f %.1f %.1f alpha %.1f, non-black %.1f%%" % (
            name, rgb[..., 0].mean(), rgb[..., 1].mean(), rgb[..., 2].mean(), a[..., 3].mean(), (rgb.sum(2) > 8).mean() * 100))
    cells.append((name, Image.fromarray(rgb.copy()).convert("RGB")))
cols = 3
cw, ch = 426, 256
rows = math.ceil(len(cells) / cols) or 1
sheet = Image.new("RGB", (cw * cols, ch * rows), (40, 0, 40))
draw = ImageDraw.Draw(sheet)
for i, (name, im) in enumerate(cells):
    im.thumbnail((cw - 4, ch - 18))
    x, y = (i % cols) * cw, (i // cols) * ch
    sheet.paste(im, (x + 2, y + 16))
    draw.text((x + 3, y + 2), name[:66], fill=(255, 255, 0))
sheet.save(sys.argv[2])
print("wrote", sys.argv[2], len(cells), "images")
