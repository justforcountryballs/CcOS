import sys, numpy as np
from PIL import Image, ImageDraw, ImageFilter
sys.path.insert(0, '/root/myos/wallgen')
from noise import *
W, H = int(sys.argv[1]), int(sys.argv[2]); S = W / 1920
SS = 2; w, h = W * SS, H * SS
rng = np.random.default_rng(51)
base = Image.new('RGB', (w, h), (120, 40, 170))
img = base.copy()
ext = np.array([-14, 22.0]) * S * SS                         # the slabs' thickness, seen from above-right
def slab(cx, cy, ang, L, Wd):
    d = np.array([np.cos(ang), np.sin(ang)]); n = np.array([-d[1], d[0]])
    c = np.array([cx, cy]) * [w, h]
    p = [c - d * L / 2 - n * Wd / 2, c + d * L / 2 - n * Wd / 2, c + d * L / 2 + n * Wd / 2, c - d * L / 2 + n * Wd / 2]
    return [tuple(q) for q in p]
slabs = []
for i in range(95):                                          # a fan of long slabs from the lower left
    t = i / 95
    ang = -0.15 - t * 1.25 + rng.normal(0, 0.06)
    r = 0.15 + rng.random() * 1.35
    cx = 0.05 + np.cos(ang + 0.2) * r * 0.9 + rng.normal(0, 0.05)
    cy = 1.05 + np.sin(ang + 0.2) * r * 0.9
    L = (0.25 + rng.random() * 0.5) * w; Wd = (0.02 + rng.random() ** 2 * 0.12) * w
    slabs.append((rng.random(), cx, cy, ang, L, Wd))
slabs.sort()
for k, (_, cx, cy, ang, L, Wd) in enumerate(slabs):
    p = slab(cx, cy, ang, L, Wd)
    sh = Image.new('L', (w, h), 0); ImageDraw.Draw(sh).polygon([(x + ext[0] * 1.6, y + ext[1] * 1.6) for x, y in p], fill=150)
    sh = sh.filter(ImageFilter.GaussianBlur(18 * S * SS))
    img = Image.composite(Image.new('RGB', (w, h), (40, 8, 60)), img, sh)
    dr = ImageDraw.Draw(img)
    pe = [(x + ext[0], y + ext[1]) for x, y in p]
    for a in range(4):                                       # the sides
        b = (a + 1) % 4
        quad = [p[a], p[b], pe[b], pe[a]]
        ex = np.array(p[b]) - np.array(p[a]); nn = np.array([ex[1], -ex[0]]); nn /= np.linalg.norm(nn) + 1e-9
        lit = 0.35 + 0.4 * max(0, nn @ np.array([0.6, -0.8]))
        dr.polygon(quad, fill=tuple(int(v * lit) for v in (190, 70, 230)))
    v = 0.85 + rng.random() * 0.3
    top = tuple(int(min(255, c * v)) for c in (178, 72, 222))
    dr.polygon(p, fill=top)
    dr.line(p + [p[0]], fill=tuple(min(255, int(c * 1.15)) for c in top), width=max(1, int(1.5 * S * SS)))
img = img.resize((W, H), Image.LANCZOS)
a = np.asarray(img).astype(float) / 255
yy, xx = np.mgrid[0:H, 0:W] / W; Y = yy * W / H
light = 0.75 + 0.45 * np.exp(-((xx - 0.85) ** 2 + (Y - 0.3) ** 2) * 2.5)    # light from the upper right
a = a * light[..., None]
save(a, sys.argv[3])
