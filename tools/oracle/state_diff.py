#!/usr/bin/env python3
"""Generic labeled key=val state diff (recomp vs Mesen). Used for both the CPU
register dump (.cpu) and the PSG latched-register dump (.psg). Reports per-key
match/mismatch + a verdict. Keys listed in --jitter are flagged as expected to
differ from sub-cycle sampling (e.g. lfsr, pc, r) and don't fail the verdict.

Usage: python state_diff.py recomp.txt mesen.txt [--label cpu] [--jitter lfsr,pc,r] [--json out]
"""
import sys, json, argparse


def load(p):
    d = {}
    for line in open(p):
        line = line.strip()
        if "=" in line:
            k, v = line.split("=", 1)
            try: d[k] = int(v)
            except ValueError: d[k] = v
    return d


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("recomp"); ap.add_argument("mesen")
    ap.add_argument("--label", default="state")
    ap.add_argument("--jitter", default="", help="comma keys allowed to differ (sub-cycle)")
    ap.add_argument("--json", default=None)
    a = ap.parse_args()
    jitter = set(x for x in a.jitter.split(",") if x)
    r, m = load(a.recomp), load(a.mesen)
    keys = sorted(set(r) | set(m))

    hard, soft = [], []
    for k in keys:
        rv, mv = r.get(k), m.get(k)
        if rv != mv:
            (soft if k in jitter else hard).append({"key": k, "recomp": rv, "mesen": mv})

    verdict = "MATCH" if not hard else "MISMATCH"
    res = {"label": a.label, "keys": len(keys),
           "hard_mismatch": hard, "soft_mismatch": soft, "verdict": verdict}

    print(f"  [{a.label}] {len(keys)} keys | {len(hard)} hard-mismatch, "
          f"{len(soft)} jitter-soft")
    for d in hard:
        print(f"    MISMATCH {d['key']}: recomp={d['recomp']} mesen={d['mesen']}")
    for d in soft:
        print(f"    (soft)   {d['key']}: recomp={d['recomp']} mesen={d['mesen']}")
    print(f"  VERDICT: {verdict}")
    if a.json:
        json.dump(res, open(a.json, "w"), indent=2)
    return 0


if __name__ == "__main__":
    sys.exit(main())
