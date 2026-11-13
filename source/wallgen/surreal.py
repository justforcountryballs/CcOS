import sys, numpy as np
sys.path.insert(0, '/root/myos/wallgen')
from noise import *; from land import *
W, H = int(sys.argv[1]), int(sys.argv[2]); S = W / 1920
yy, xx = np.mgrid[0:H, 0:W] / W; Y = yy * W / H
sky = grad([(0, '23508f'), (0.45, '5a8fc6'), (0.68, 'a9c3d6'), (0.76, 'f1c896'), (0.8, 'f5a35e')], Y)
water_line = int(H * 0.78)
img = sky.copy()
lit = col('d58a5c'); shade = col('241d3a'); mid = col('6a4a5c')
def mountains(img, top, seed, haze):
    r, g = rock(W, H, S, seed)
    slope = np.gradient(gaussian_filter(top, 25 * S))[None, :] * W * 0.8   # face turned to the right: lit
    l = np.clip(0.45 + g * 0.35 + np.tanh(-slope) * 0.35, 0, 1) * (0.6 + r * 0.6)
    depth = np.clip((Y - top[None, :]) * 2.5, 0, 1)
    c = grad([(0, '1d1730'), (0.35, '4e3550'), (0.7, 'b06a4e'), (1, 'f0a36e')], l * (1 - depth * 0.35))
    c = mix(c, sky, haze)
    m = smooth(-0.002, 0.002, Y - top[None, :]) * (Y < water_line / H + 0.01)
    return mix(img, c, m)
far = ridge1d(W, 41, [(0.08, 0.06, 0.08), (0.18, 0.05, 0.05)], 0.79, 1.0, S, 0.03)
img = mountains(img, far, 42, 0.45)
near = ridge1d(W, 43, [(0.37, 0.27, 0.07), (0.55, 0.40, 0.09), (0.72, 0.36, 0.08), (0.86, 0.33, 0.12)], 0.8, 1.0, S, 0.05)
img = mountains(img, near, 44, 0.0)
# the upside-down mountain hanging from the top
hang = 1 - ridge1d(W, 45, [(0.40, 0.33, 0.055), (0.22, 0.2, 0.07), (0.56, 0.2, 0.06)], 0.98, 1.0, S, 0.05)   # bottom edge, from the top
hang = hang * (1 - smooth(0.6, 0.8, np.arange(W) / W) * 1.0)
r, g = rock(W, H, S, 46)
l = np.clip(0.4 + g * 0.35, 0, 1) * (0.6 + r * 0.6)
lift = np.clip((hang[None, :] - Y) * 3, 0, 1)
c = grad([(0, '1a1428'), (0.4, '4a3048'), (0.75, 'a8603f'), (1, 'e89a62')], l * (0.5 + 0.5 * (1 - lift)))
m = smooth(0.002, -0.002, Y - hang[None, :])
img = mix(img, c, m)
img = mirror(img, water_line, S, 47, 0.5, 0.55, col('101828'))
save(img, sys.argv[3])
