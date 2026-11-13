import numpy as np
from noise import *
def ridge1d(W, seed, peaks, base, amp, S, rough=0.04):
    """a mountain skyline: smooth peaks plus jagged detail, height measured from the top (0) down"""
    x = np.arange(W) / W
    h = np.full(W, base)
    for px, ph, pw in peaks:                                  # each peak: a sharp-ish cone
        h = np.minimum(h, base - ph * np.exp(-np.abs(x - px) ** 1.2 / pw))
    rng = np.random.default_rng(seed)
    det = np.zeros(W)
    for o in range(6):
        n = gaussian_filter(rng.standard_normal(W), (60 / 2 ** o) * S, mode='wrap'); n /= n.std() + 1e-9
        det += -np.abs(n) * 0.5 ** o                              # ridged: sharp crests
    det = (det - det.min()) / (det.max() - det.min() + 1e-9) - 1   # -1..0
    return h - det * rough * amp
def rock(W, H, S, seed, light=1.0, vstretch=0.25):
    """rock-face shading: long streaks running down the mountain, lit from one side"""
    rng = np.random.default_rng(seed)
    out = np.zeros((H, W)); a = 1.0
    for o in range(4):
        s = 60 * S / 2 ** o
        n = gaussian_filter(rng.standard_normal((H, W)), [s / vstretch * 0.25, s * 0.25]); n /= n.std() + 1e-9
        out += (1 - np.abs(n)) * a; a *= 0.55
    gx = np.gradient(gaussian_filter(out, 1.0 * S), axis=1)
    return out / 2.2, np.tanh(gx * light * 10 / S)
def mirror(img, line, S, seed, ripple=1.0, fade=0.75, tint=None):
    """the water: what's above `line` (a row) mirrored below it, with ripples"""
    H, W = img.shape[:2]
    out = img.copy()
    rows = np.arange(line, H)
    rng = np.random.default_rng(seed)
    wav = gaussian_filter(rng.standard_normal((H, W)), [1.2 * S, 30 * S]); wav /= wav.std() + 1e-9
    for y in rows:
        d = y - line
        src = line - d - 1
        if src < 0: src = 0
        sh = (wav[y] * (1.5 + d * 0.025) * ripple * S).astype(int)
        xs = np.clip(np.arange(W) + sh, 0, W - 1)
        sy = np.clip(src + (wav[y] * 0.6 * ripple * (1 + d * 0.02)).astype(int), 0, line - 1)
        out[y] = img[sy, xs]
    k = np.clip((rows - line) / (H - line), 0, 1)[:, None, None]
    out[line:] = out[line:] * fade
    if tint is not None: out[line:] = mix(out[line:], tint, k * 0.6)
    return out
