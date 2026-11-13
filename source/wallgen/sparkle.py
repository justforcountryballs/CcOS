import sys, numpy as np
sys.path.insert(0, '/root/myos/wallgen')
from noise import *
W, H = int(sys.argv[1]), int(sys.argv[2]); S = W / 1920
yy, xx = np.mgrid[0:H, 0:W] / W; Y = yy * W / H
dust = field(W, H, 70 * S, 21, 4, 0.55)
band = Y + dust * 0.35 + np.sin(xx * 6 + 1) * 0.06            # wavy layers, pink above, blue below
base = grad([(0, '3a1450'), (0.22, '8a3aa8'), (0.42, 'd57ad8'), (0.55, '7a3aa8'), (0.7, '2a3a9a'), (0.85, '2a6fd0'), (1, '0a1840')], band)
glow = smooth(0.0, 0.8, field(W, H, 40 * S, 22, 3) + 0.3) * smooth(0.6, 0.2, np.abs(band - 0.5))
img = base * (0.7 + 0.6 * glow[..., None])
rng = np.random.default_rng(23)
pts = np.zeros((H, W)); big = np.zeros((H, W))
n = int(25000 * S * S)
px = rng.integers(0, W, n); py = rng.integers(0, H, n)
keep = rng.random(n) < (0.25 + 0.75 * glow[py, px])           # more sparkles in the bright dust
px, py = px[keep], py[keep]
b = rng.random(px.size) ** 3
np.add.at(pts, (py, px), b * 2.5)
m = rng.random(px.size) < 0.05
np.add.at(big, (py[m], px[m]), 1.0)
sp = gaussian_filter(pts, 0.7 * S) * 4 + gaussian_filter(big, 5 * S) * 60 * S * S
sp = np.clip(sp, 0, 1.4)
img += sp[..., None] * mix(np.ones(3), col('ffd6f7'), 0.4)
img *= (1 - 0.25 * ((xx - 0.5) ** 2 * 2 + (Y - 0.5) ** 2 * 2))[..., None]
save(img, sys.argv[3])
