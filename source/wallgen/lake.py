import sys, numpy as np
sys.path.insert(0, '/root/myos/wallgen')
from noise import *; from land import *
W, H = int(sys.argv[1]), int(sys.argv[2]); S = W / 1920
SNOW = len(sys.argv) > 4 and sys.argv[4] == 'snow'
yy, xx = np.mgrid[0:H, 0:W] / W; Y = yy * W / H
sun = (0.12, 0.30) if not SNOW else (0.86, 0.40)
d = np.sqrt((xx - sun[0]) ** 2 + (Y - sun[1]) ** 2)
if SNOW:
    sky = grad([(0, '3a4a7a'), (0.25, '8a6aa0'), (0.45, 'f08a7a'), (0.6, 'ffb070')], Y)
    sn = gaussian_filter(np.random.default_rng(61).standard_normal((H, W)), [6 * S, 90 * S]); sn /= sn.std()
    cl = smooth(0.3, 1.4, sn + field(W, H, 120 * S, 62) * 1.2) * smooth(0.55, 0.2, Y)
    sky = mix(sky, grad([(0, '7a4a78'), (0.5, 'e87a70'), (1, 'ffc890')], 1.2 - Y * 2 + sn * 0.15), cl * 0.85)
else:
    sky = grad([(0, '2a6fc0'), (0.3, '6fa8dc'), (0.5, 'cfe0e8'), (0.6, 'f6e0b0')], Y)
glow = np.exp(-d * 60) * 1.4 + np.exp(-d * 9) * 0.45 + np.exp(-d * 2.5) * 0.15
rays = (0.5 + 0.5 * np.cos(np.arctan2(Y - sun[1], xx - sun[0]) * 22)) ** 10 * np.exp(-d * 9) * 0.15
img = sky + ((glow + rays) * (1 if not SNOW else 0.6))[..., None] * col('fff1c4')
water = int(H * 0.60) if not SNOW else int(H * 0.58)
def mount(img, top, seed, cols, haze, light_from_left=True, snow=0.0, snowline=0.0):
    r, g = rock(W, H, S, seed)
    sl = np.gradient(gaussian_filter(top, 20 * S))[None, :] * W
    face = np.tanh(sl * 0.6) * (-1 if light_from_left else 1)    # facing the sun
    l = np.clip(0.42 + face * 0.42 + g * 0.35 * (1 if light_from_left else -1), 0, 1) * (0.55 + r * 0.7)
    c = grad(cols, l)
    if snow:
        sn = smooth(0.0, 0.08, snowline - (Y - top[None, :]) + (r - 0.5) * 0.12) * snow
        c = mix(c, grad([(0, '7d8fb8'), (0.6, 'e6e8f4'), (1, 'fff4ea')], l), sn)
    c = mix(c, sky, haze)
    m = smooth(-0.0015, 0.0015, Y - top[None, :])
    return mix(img, c, m)
if SNOW:
    img = mount(img, ridge1d(W, 63, [(0.62, 0.14, 0.12), (0.88, 0.1, 0.1)], 0.6, 1, S, 0.02), 64,
                [(0, '4a4a78'), (1, 'b0a0c8')], 0.45, False, 1.0, 0.2)
    img = mount(img, ridge1d(W, 65, [(0.36, 0.38, 0.07), (0.15, 0.2, 0.1), (0.55, 0.15, 0.08)], 0.6, 1, S, 0.03), 66,
                [(0, '30344f'), (0.5, '6a6a8a'), (1, 'ffd2b0')], 0.0, False, 1.0, 0.35)
else:
    img = mount(img, ridge1d(W, 67, [(0.55, 0.34, 0.06), (0.68, 0.26, 0.05), (0.80, 0.3, 0.07), (0.38, 0.2, 0.08)], 0.6, 1, S, 0.06), 68,
                [(0, '6d6a80'), (0.6, 'c8a49a'), (1, 'f7c9a6')], 0.3, True, 0.7, 0.05)
# forest: dark hills with pointed tree tops
rng = np.random.default_rng(69)
x = np.arange(W) / W
for k, (lvl, hz, dens) in enumerate([(0.555, 0.35, 260), (0.585, 0.12, 180)]):
    hill = lvl + field(W, 1, 160 * S, 70 + k)[0] * 0.05 + (0.04 if not SNOW else 0.05) * np.exp(-((x - (0.42 if not SNOW else 0.1)) / 0.12) ** 2) * -1
    tops = hill.copy()
    for i in range(dens):                                    # each tree: a narrow spike
        tx = rng.random(); th = 0.012 + rng.random() * 0.03; tw = th * 0.28
        prof = hill - th * np.clip(1 - np.abs(x - tx) / tw, 0, 1)
        tops = np.minimum(tops, prof)
    tree = smooth(-0.001, 0.001, Y - tops[None, :])
    if SNOW:
        tc = grad([(0, '2a1812'), (0.5, '7a3a1a'), (1, 'd87a30')], np.clip(field(W, H, 14 * S, 72 + k) * 0.6 + 0.45 - (Y - tops[None, :]) * 6, 0, 1))
    else:
        rim = np.exp(-(Y - tops[None, :]) / 0.004) * np.exp(-np.abs(xx - sun[0]) * 3)
        tc = grad([(0, '0c2016'), (0.5, '24452a'), (1, 'd8c070')], np.clip(field(W, H, 14 * S, 72 + k) * 0.4 + 0.3 - (Y - tops[None, :]) * 5 + rim * 0.8, 0, 1))
    tc = mix(tc, sky, hz)
    img = mix(img, tc, tree * (Y < water / H + 0.005))
# the lake
img = mirror(img, water, S, 75, 0.35, 0.85)
k = np.clip((Y - water / H) / (1 - water / H), 0, 1)
if SNOW:
    rr = np.random.default_rng(76)
    for i in range(9):
        cx = rr.random(); cy = (water / H + 0.22 + rr.random() ** 0.5 * 0.3) * H / W; rw = (0.02 + rr.random() * 0.05) * (0.5 + (cy * W / H - 0.6) * 2)
        e = ((xx - cx) / rw) ** 2 + ((yy - cy) / (rw * 0.45)) ** 2
        m = smooth(1.0, 0.92, e + field(W, H, 4 * S, 79 + i) * 0.15)
        shade = np.clip(0.25 + (cy - yy) / (rw * 0.45) * 0.35 + (xx - cx) / rw * 0.25, 0, 1)
        img = mix(img, grad([(0, '15121a'), (0.6, '4a3c44'), (1, 'e8a080')], shade), m)
else:
    img = mix(img, col('1fa59a'), smooth(0.35, 1.0, k)[..., None] * 0.55)
    peb = np.clip(field(W, H, 10 * S, 78, 2) * 0.7 + 0.5, 0, 1)
    img = mix(img, grad([(0, '2a6a62'), (1, '7ab8a8')], peb), smooth(0.7, 1.0, k) * 0.3)
vig = 1 - 0.25 * ((xx - 0.5) ** 2 * 1.5 + (Y - 0.5) ** 2 * 2)
save(img * vig[..., None], sys.argv[3])
