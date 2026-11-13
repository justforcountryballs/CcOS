import sys, numpy as np
sys.path.insert(0, '/root/myos/wallgen')
from noise import *
W, H = int(sys.argv[1]), int(sys.argv[2]); S = W / 1920
yy, xx = np.mgrid[0:H, 0:W] / W; Y = yy * W / H
sun = np.array([0.52, 0.53])
d = np.sqrt((xx - sun[0]) ** 2 + ((Y - sun[1]) * 1.8) ** 2)
sky = grad([(0, '8d9bb2'), (0.22, 'b9b0a6'), (0.4, 'efb978'), (0.5, 'ffd27a'), (0.58, 'f6a04c'), (1, 'c0602e')], Y)
sky = sky + (np.exp(-d * 10) * 0.8 + np.exp(-d * 3) * 0.25)[..., None] * col('fff2c8')
sn = gaussian_filter(np.random.default_rng(31).standard_normal((H, W)), [3 * S, 70 * S]); sn /= sn.std()
st = smooth(0.6, 1.6, sn + field(W, H, 150 * S, 39) * 1.5)
sky = mix(sky, sky * col('eaa275') * 1.12, st * smooth(0.5, 0.05, Y) * 0.6)
rng = np.random.default_rng(36)
img = sky.copy()
noise2 = field(W, H, 9 * S, 40, 3)
for layer, (base, rmin, rmax, haze, count) in enumerate([(0.60, 0.015, 0.04, 0.65, 90), (0.66, 0.025, 0.06, 0.4, 70), (0.76, 0.04, 0.10, 0.12, 50)]):
    m = np.zeros((H, W)); dep = np.ones((H, W))
    puffs = []
    for i in range(count):
        r = rmin + (rmax - rmin) * rng.random() ** 1.6
        puffs.append(((base + rng.random() * 0.04) * H / W + r * 0.5, rng.random() * 1.2 - 0.1, r))
    puffs.sort()
    for cy, cx, r in puffs:                                  # lower puffs are nearer: drawn last
        x0, x1 = max(0, int((cx - r * 1.1) * W)), min(W, int((cx + r * 1.1) * W) + 1)
        y0, y1 = max(0, int((cy - r * 1.1) * W)), H
        if x0 >= x1 or y0 >= y1: continue
        sx, sy = xx[y0:y1, x0:x1], yy[y0:y1, x0:x1]
        dd = np.sqrt((sx - cx) ** 2 + np.minimum(sy - cy, 0) ** 2 * 1.3)   # round top, filled below
        inside = smooth(r * 1.02, r * 0.97, dd + noise2[y0:y1, x0:x1] * r * 0.18)
        dp = np.clip((sy - (cy - r)) / (r * 1.6), 0, 1)
        mm = m[y0:y1, x0:x1]
        dep[y0:y1, x0:x1] = dep[y0:y1, x0:x1] * (1 - inside) + dp * inside
        m[y0:y1, x0:x1] = np.maximum(mm, inside)
    near_sun = np.exp(-np.abs(xx - sun[0]) * 3.5)
    c = grad([(0, 'ffdcaa'), (0.12, 'e3a47c'), (0.4, '9a6c6c'), (1, '4c3f50')], dep * (1.1 - 0.25 * near_sun) + noise2 * 0.12 + layer * 0.12)
    c = mix(c, sky, haze * 0.7)
    rim = np.exp(-dep / 0.035) * (0.35 + near_sun) * m
    img = mix(img, c, m)
    img += (rim * 0.45 * (1 - haze * 0.6))[..., None] * col('ffe4b8')
save(img, sys.argv[3])
