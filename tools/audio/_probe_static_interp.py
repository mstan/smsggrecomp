import numpy as np, sys, audio_diff as ad
out = sys.argv[1]
sr1, a = ad.load_wav(out + r"\recomp_static.wav")
sr2, b = ad.load_wav(out + r"\recomp_interp.wav")
am = a.mean(1); bm = b.mean(1)
n = min(len(am), len(bm)); am, bm = am[:n], bm[:n]
print("native rate", sr1, "samples", n)
print("raw native NCC lag0 =", round(ad.ncc(am, bm), 4))
diff = np.sum(np.any(a[:n] != b[:n], axis=1))
print("differing stereo frames =", int(diff), "of", n,
      "(", round(100.0 * diff / n, 2), "% )")
# magnitude of difference where they differ
d = (am - bm)
print("RMS(diff)/RMS(ref) =", round(np.sqrt(np.mean(d * d)) / (np.sqrt(np.mean(bm * bm)) + 1e-20), 4))
# sample-precise best lag within +/-50 samples (native rate)
best = (0, -9.0)
for L in range(-50, 51):
    if L >= 0:
        x, y = am[L:], bm[:n - L]
    else:
        x, y = am[:n + L], bm[-L:]
    m = min(len(x), len(y)); c = ad.ncc(x[:m], y[:m])
    if c > best[1]:
        best = (L, c)
print("best native lag", best[0], "samples -> NCC", round(best[1], 4))
# first divergence frame
nz = np.where(np.any(a[:n] != b[:n], axis=1))[0]
print("first differing frame =", int(nz[0]) if len(nz) else None,
      "(t=", round((int(nz[0]) / sr1) if len(nz) else 0, 4), "s )")
