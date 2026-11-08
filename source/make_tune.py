# ccos startup tune: a soft, original fade-in tune (synthesized, no samples)
import numpy as np, wave

SR = 44100
DUR = 5.4
t = np.arange(int(SR * DUR)) / SR
L = np.zeros_like(t); R = np.zeros_like(t)
rng = np.random.default_rng(11)

def hz(n): return 440.0 * 2 ** ((n - 69) / 12)

def env(start, attack, hold, release):
    """smooth rise over `attack`, stay `hold`, then fall over `release` (all seconds)"""
    e = np.zeros_like(t)
    tt = t - start
    up = np.clip(tt / attack, 0, 1); up = up * up * (3 - 2 * up)
    down = np.clip(1 - (tt - attack - hold) / release, 0, 1); down = down * down * (3 - 2 * down)
    m = tt >= 0
    e[m] = (up * down)[m]
    return e

def add(sig, pan):
    global L, R
    L += sig * np.sqrt((1 - pan) / 2); R += sig * np.sqrt((1 + pan) / 2)

# 1) the pad: F major 9 (F2 C3 A3 E4 G4), swelling in slowly from silence
for n, pan, amp in [(41, 0.0, 0.11), (48, -0.35, 0.08), (57, 0.35, 0.07), (64, -0.2, 0.05), (67, 0.25, 0.045)]:
    f = hz(n)
    s = np.zeros_like(t)
    for d in (0.996, 1.0, 1.004):                                 # detuned trio = soft chorus
        ph = rng.uniform(0, 6.28)
        s += np.sin(2 * np.pi * f * d * t + ph) + 0.18 * np.sin(2 * np.pi * 2 * f * d * t + ph)
    wob = 1 + 0.06 * np.sin(2 * np.pi * 0.35 * t + rng.uniform(0, 6))   # slow breathing
    add(s / 3 * amp * wob * env(0.0, 1.9, 1.6, 1.8), pan)

# 2) the melody: soft "felt piano" notes A4 C5 E5 G5 -> resolve on F5
def soft_note(n, start, amp, pan, length=1.6):
    f = hz(n)
    sig = np.zeros_like(t)
    for ratio, a in [(1, 1.0), (2, 0.28), (3, 0.08), (4, 0.03)]:
        sig += a * np.sin(2 * np.pi * f * ratio * t)
    tt = np.clip(t - start, 0, None)
    shape = (1 - np.exp(-tt / 0.035)) * np.exp(-tt / (length * 0.55))  # gentle attack, long decay
    shape[t < start] = 0
    add(sig * shape * amp, pan)

melody = [(69, 0.55, -0.25), (72, 0.95, 0.1), (76, 1.35, 0.3), (79, 1.80, -0.1), (77, 2.45, 0.05)]
for i, (n, st, pan) in enumerate(melody):
    last = i == len(melody) - 1
    soft_note(n, st, 0.11 if not last else 0.12, pan, 2.6 if last else 1.4)
soft_note(65, 2.45, 0.05, -0.2, 2.6)                             # low F under the resolution
soft_note(84, 2.75, 0.025, 0.4, 2.0)                             # a faint high C sparkle

# 3) a breath of air: soft noise that swells with the pad
noise = rng.standard_normal(len(t))
spec = np.fft.rfft(noise); fr = np.fft.rfftfreq(len(t), 1 / SR)
spec *= np.exp(-((fr - 2500) / 1800) ** 2)                       # keep a soft band around 2.5 kHz
air = np.fft.irfft(spec, len(t)); air /= np.abs(air).max()
add(air * 0.025 * env(0.2, 1.8, 0.8, 1.6), 0.0)

# 4) warm it up (roll off harsh highs) and add a big soft reverb
def warm(x):
    X = np.fft.rfft(x); fr = np.fft.rfftfreq(len(x), 1 / SR)
    return np.fft.irfft(X / np.sqrt(1 + (fr / 5000) ** 4), len(x))
def reverb(x, seed, decay=0.9):
    n = int(SR * 3.0); tt = np.arange(n) / SR
    ir = np.random.default_rng(seed).standard_normal(n) * np.exp(-tt / decay)
    ir[:int(SR * 0.02)] = 0
    ir /= np.sqrt(np.sum(ir ** 2))
    size = 1 << int(np.ceil(np.log2(len(x) + n)))
    return np.fft.irfft(np.fft.rfft(x, size) * np.fft.rfft(ir, size), size)[:len(x)]
L, R = warm(L), warm(R)
L2 = L + 0.55 * warm(reverb(L, 3)); R2 = R + 0.55 * warm(reverb(R, 4))

# 5) overall: fade in from silence, long fade out, normalize to -4 dB
master = np.clip(t / 0.8, 0, 1) ** 1.5 * np.clip((DUR - t) / 1.6, 0, 1) ** 1.2
L2 *= master; R2 *= master
g = 10 ** (-4 / 20) / max(np.abs(L2).max(), np.abs(R2).max())
pcm = (np.clip(np.stack([L2 * g, R2 * g], 1), -1, 1) * 32767).astype('<i2')
with wave.open('startup.wav', 'wb') as w:
    w.setnchannels(2); w.setsampwidth(2); w.setframerate(SR); w.writeframes(pcm.tobytes())
print('wrote startup.wav', round(len(t) / SR, 2), 's')
