import sys, numpy as np
sys.path.insert(0, '/root/myos/wallgen')
from noise import *
W, H = int(sys.argv[1]), int(sys.argv[2]); S = W / 1920
yy, xx = np.mgrid[0:H, 0:W] / W
# the liquid: folded ribbons, bent by slow noise
wx = field(W, H, 220 * S, 11) * 0.30; wy = field(W, H, 260 * S, 12) * 0.30
u = xx + wx; v = yy + wy
r1 = np.sin(u * 13.0 + np.sin(v * 7.0 + 1.0) * 2.4)
r2 = np.sin(v * 17.0 - u * 6.0 + np.cos(u * 5.0) * 2.5)
h = r1 * 0.6 + r2 * 0.4
h = np.tanh(h * 1.6)
mask = smooth(-0.15, 0.35, field(W, H, 200 * S, 13, 2))   # parts where the liquid is, flat elsewhere
h = h * (0.25 + 0.75 * mask)
h = gaussian_filter(h, 2.0 * S)
gy, gx = np.gradient(h * 140 * S)
n = np.dstack([-gx, -gy, np.ones_like(h)]); n /= np.linalg.norm(n, axis=2, keepdims=True)
# reflect a bright studio: white light strip up-left, blue below
V = np.array([0, 0, 1.0])
rr = 2 * n[..., 2:3] * n - V
ry = rr[..., 1]; rx = rr[..., 0]
env = grad([(0, 'ffffff'), (0.35, 'e3eefb'), (0.55, 'acd0f3'), (0.8, '5d97e0'), (1, '2f6cc4')], 0.5 + ry * 0.9 + rx * 0.25)
strip = np.exp(-((ry + 0.35 + rx * 0.4) / 0.08) ** 2) + 0.6 * np.exp(-((rx - 0.45 - ry * 0.3) / 0.06) ** 2)
fres = 1 - n[..., 2]
bg = grad([(0, 'f2f7fd'), (0.5, 'd3e3f6'), (1, 'a8c7ec')], yy * W / H * 0.8 + xx * 0.25)
glass = np.clip(fres * 9, 0, 1)                         # glass where the surface bends
img = mix(bg, env, glass * 0.85)
img = mix(img, col('2a66c2'), np.clip(fres * 12 - 2.2, 0, 1) * 0.38)   # deep blue in steep folds
img += (strip * glass)[..., None] * 0.55
img += (np.clip(n[..., 0] * -0.6 + n[..., 1] * -0.8, 0, 1) ** 6 * glass)[..., None] * 0.6
# depth of field: sharp across the middle, soft towards the corners
far = np.clip(np.abs(yy * W / H - 0.5 - (xx - 0.5) * 0.35) * 2.6 - 0.35, 0, 1)
blur = np.dstack([gaussian_filter(img[..., c], 9 * S) for c in range(3)])
img = mix(img, blur, far)
vig = 1 - 0.15 * (((xx - 0.5) * 1.5) ** 2 + ((yy * W / H - 0.5) * 1.8) ** 2)
img *= vig[..., None]
save(img, sys.argv[3])
