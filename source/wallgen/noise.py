import numpy as np
from scipy.ndimage import gaussian_filter, map_coordinates
def field(w, h, scale, seed, octaves=1, persist=0.5):
    """smooth random field in about [-1, 1]: blurred noise, several sizes added up"""
    rng = np.random.default_rng(seed)
    out = np.zeros((h, w)); amp = 1.0; tot = 0
    for o in range(octaves):
        s = scale / (2 ** o)
        n = rng.standard_normal((h, w))
        n = gaussian_filter(n, s, mode='wrap')
        n /= n.std() + 1e-9
        out += amp * n; tot += amp; amp *= persist
    return out / tot / 2.2
def warp(img, dx, dy):
    h, w = img.shape[:2]
    yy, xx = np.mgrid[0:h, 0:w].astype(float)
    if img.ndim == 2: return map_coordinates(img, [yy + dy, xx + dx], order=1, mode='reflect')
    return np.dstack([map_coordinates(img[..., c], [yy + dy, xx + dx], order=1, mode='reflect') for c in range(img.shape[2])])
def smooth(e0, e1, x):
    t = np.clip((x - e0) / (e1 - e0), 0, 1); return t * t * (3 - 2 * t)
def mix(a, b, t):
    t = np.asarray(t)[..., None] if np.ndim(t) == 2 else t
    return a * (1 - t) + b * t
def col(hexs): return np.array([int(hexs[i:i + 2], 16) for i in (0, 2, 4)], float) / 255
def grad(stops, t):
    """colour ramp: stops = [(pos, 'rrggbb'), ...]"""
    t = np.clip(t, 0, 1); out = np.zeros(t.shape + (3,))
    for (p0, c0), (p1, c1) in zip(stops, stops[1:]):
        m = (t >= p0) & (t <= p1); k = ((t - p0) / (p1 - p0))[m][..., None]
        out[m] = col(c0) * (1 - k) + col(c1) * k
    return out
def save(img, path):
    from PIL import Image
    a = np.clip(img, 0, 1) ** (1 / 1.0)
    a = a + (np.random.default_rng(1).random(a.shape) - 0.5) / 255   # dither: no banding
    Image.fromarray((np.clip(a, 0, 1) * 255 + 0.5).astype(np.uint8)).save(path)
