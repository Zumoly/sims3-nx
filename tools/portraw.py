"""Converts host surface dumps (portrait*.raw, step*.raw: linear RGBA8, size in the name) to PNG.
Writes <name>.png (colour on a grey checker, so transparency shows) and <name>_a.png (alpha).
usage: python portraw.py <file.raw> [...]"""
import re, sys

import numpy as np
from PIL import Image

for path in sys.argv[1:]:
    m = re.search(r"_(\d+)x(\d+)\.raw$", path)
    if not m:
        print("no size in", path)
        continue
    w, h = int(m.group(1)), int(m.group(2))
    px = np.fromfile(path, np.uint8)[: w * h * 4].reshape(h, w, 4)
    ys, xs = np.mgrid[0:h, 0:w]
    checker = np.where(((xs >> 3) + (ys >> 3)) & 1, 160, 96).astype(np.float32)[..., None]
    alpha = px[..., 3:4].astype(np.float32) / 255.0
    over = px[..., :3] * alpha + checker * (1 - alpha)
    base = path[:-4]
    Image.fromarray(over.astype(np.uint8), "RGB").save(base + ".png")
    Image.fromarray(px[..., 3], "L").save(base + "_a.png")
    print(path, "rgb min/max", px[..., :3].min(), px[..., :3].max(), "alpha min/max", px[..., 3].min(), px[..., 3].max())
