#!/usr/bin/env python3
"""Offset-independent per-anchor cycle comparison: recomp vs Mesen.

Both sides record (hit_index, guest_cycle) each time the CPU reaches an anchor
PC (recomp: `--cyc-watch ADDR file`; Mesen: mesen_cyc_watch.lua). Absolute
cycles differ by a boot offset that varies run-to-run, so we compare the DELTA
between consecutive same-anchor hits -- which cancels the offset entirely (the
psxrecomp cycle_compare lesson). If the two delta sequences match, the recomp's
inter-anchor timing equals the accurate oracle's.

Reports: best integer hit-alignment, per-side delta stats, the histogram of
(recomp_delta - mesen_delta), the first diverging hit, and a verdict.

Usage:
  python cyc_compare.py recomp.csv mesen.csv [--max-shift 8] [--tol 0] [--json out]
"""
import argparse, json, sys
import numpy as np


def load(path):
    rows = [l.strip().split(",") for l in open(path) if l.strip()]
    rows = [r for r in rows if r and r[0] not in ("hit",)]
    cyc = np.array([int(r[1]) for r in rows], dtype=np.int64)
    return cyc


def deltas(cyc):
    return np.diff(cyc)


def best_shift(dr, dm, max_shift):
    """integer shift s applied to mesen so dr[i] ~ dm[i+s], minimizing L1."""
    best = (0, None)
    for s in range(-max_shift, max_shift + 1):
        if s >= 0:
            a, b = dr[: len(dr) - s], dm[s:]
        else:
            a, b = dr[-s:], dm[: len(dm) + s]
        n = min(len(a), len(b))
        if n < 8:
            continue
        l1 = float(np.mean(np.abs(a[:n] - b[:n])))
        if best[1] is None or l1 < best[1]:
            best = (s, l1)
    return best


def analyze(recomp_csv, mesen_csv, max_shift, tol):
    cr, cm = load(recomp_csv), load(mesen_csv)
    dr, dm = deltas(cr), deltas(cm)
    s, _ = best_shift(dr, dm, max_shift)
    if s >= 0:
        a, b = dr[: len(dr) - s], dm[s:]
    else:
        a, b = dr[-s:], dm[: len(dm) + s]
    n = min(len(a), len(b))
    a, b = a[:n], b[:n]
    diff = a - b

    nz = np.where(np.abs(diff) > tol)[0]
    # histogram of differences
    vals, counts = np.unique(diff, return_counts=True)
    hist = sorted(zip(vals.tolist(), counts.tolist()), key=lambda x: -x[1])[:12]

    exact = int(np.sum(diff == 0))
    res = {
        "recomp": {"path": recomp_csv, "hits": int(len(cr))},
        "mesen": {"path": mesen_csv, "hits": int(len(cm))},
        "aligned_hit_shift": int(s),
        "compared_deltas": int(n),
        "recomp_delta": {"min": int(a.min()), "median": int(np.median(a)),
                         "max": int(a.max())},
        "mesen_delta": {"min": int(b.min()), "median": int(np.median(b)),
                        "max": int(b.max())},
        "delta_diff": {
            "exact_match": exact,
            "exact_pct": round(100.0 * exact / n, 2),
            "mean_abs": round(float(np.mean(np.abs(diff))), 3),
            "max_abs": int(np.max(np.abs(diff))),
            "first_divergence_hit": (int(nz[0]) if len(nz) else None),
        },
        "diff_histogram_top": [{"diff": int(v), "count": int(c)} for v, c in hist],
    }
    # verdict: per-anchor timing matches if the mean abs delta-diff is tiny and
    # there's no systematic drift (cumulative diff stays bounded).
    cum = float(np.sum(diff))
    res["cumulative_diff"] = int(cum)
    if res["delta_diff"]["mean_abs"] <= max(tol, 0.0) + 0.01:
        res["verdict"] = "MATCH (per-anchor timing identical)"
    elif abs(cum) < 0.5 * n:    # jitter that self-cancels, no net drift
        res["verdict"] = "JITTER-ONLY (oscillates, no net drift)"
    else:
        res["verdict"] = "DRIFT (systematic per-anchor cycle divergence)"
    return res


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("recomp"); ap.add_argument("mesen")
    ap.add_argument("--max-shift", type=int, default=8)
    ap.add_argument("--tol", type=int, default=0)
    ap.add_argument("--json", default=None)
    a = ap.parse_args()
    r = analyze(a.recomp, a.mesen, a.max_shift, a.tol)

    print(f"\n  recomp : {a.recomp}  ({r['recomp']['hits']} hits)")
    print(f"  mesen  : {a.mesen}  ({r['mesen']['hits']} hits)")
    print(f"  aligned hit-shift = {r['aligned_hit_shift']}, compared {r['compared_deltas']} deltas")
    rd, md = r["recomp_delta"], r["mesen_delta"]
    print(f"  recomp delta  min/med/max = {rd['min']}/{rd['median']}/{rd['max']}")
    print(f"  mesen  delta  min/med/max = {md['min']}/{md['median']}/{md['max']}")
    dd = r["delta_diff"]
    print(f"  delta-diff  exact={dd['exact_match']} ({dd['exact_pct']}%)  "
          f"mean|d|={dd['mean_abs']}  max|d|={dd['max_abs']}  "
          f"first_div_hit={dd['first_divergence_hit']}")
    print(f"  cumulative diff = {r['cumulative_diff']} cyc over {r['compared_deltas']} anchors")
    print(f"  diff histogram (top): " +
          ", ".join(f"{h['diff']}:{h['count']}" for h in r["diff_histogram_top"]))
    print(f"  VERDICT: {r['verdict']}\n")
    if a.json:
        json.dump(r, open(a.json, "w"), indent=2)
        print(f"  wrote {a.json}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
