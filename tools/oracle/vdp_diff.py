#!/usr/bin/env python3
"""VDP state-surface byte diff: recomp vs Mesen (Axis 4 MMIO / Axis 5 video).

Compares the raw VRAM (16 KB) and CRAM that the recomp dumps at --dump-frame
(<png>.vram / <png>.cram) against the Mesen oracle dump (mesen_vdp_dump.lua).
Byte-identical VRAM+CRAM proves the recompiled CPU's I/O-port writes ($BE/$BF
data+control) land the same bytes the accurate emulator does -- the GREEN
runtime leg for the MMIO axis. Divergence is reported as first-offset +
region (pattern table / name table / SAT) so it localizes.

Usage:
  python vdp_diff.py recomp.vram recomp.cram mesen_vram.bin mesen_cram.bin [--cram-bytes 32] [--json out]
"""
import argparse, json, sys


def load(path):
    with open(path, "rb") as f:
        return f.read()


def region(off):
    # SMS mode-4 typical layout (game-dependent, but a useful hint)
    if off < 0x3800:  return "pattern/tile data"
    if off < 0x3F00:  return "name table"
    return "SAT / misc"


def diff_bytes(a, b, label, limit_samples=8):
    n = min(len(a), len(b))
    diffs = [i for i in range(n) if a[i] != b[i]]
    first = diffs[0] if diffs else None
    samples = [{"off": i, "off_hex": f"0x{i:04X}", "recomp": a[i], "mesen": b[i],
                "region": region(i)} for i in diffs[:limit_samples]]
    return {
        "label": label,
        "len_recomp": len(a), "len_mesen": len(b), "compared": n,
        "differing": len(diffs),
        "match_pct": round(100.0 * (n - len(diffs)) / n, 4) if n else None,
        "first_divergence": (f"0x{first:04X}" if first is not None else None),
        "first_region": (region(first) if first is not None else None),
        "samples": samples,
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("recomp_vram"); ap.add_argument("recomp_cram")
    ap.add_argument("mesen_vram"); ap.add_argument("mesen_cram")
    ap.add_argument("--cram-bytes", type=int, default=32,
                    help="SMS uses 32 CRAM bytes (recomp array is 64 for GG)")
    ap.add_argument("--json", default=None)
    a = ap.parse_args()

    rv, mv = load(a.recomp_vram), load(a.mesen_vram)
    rc, mc = load(a.recomp_cram)[:a.cram_bytes], load(a.mesen_cram)[:a.cram_bytes]

    vram = diff_bytes(rv, mv, "VRAM")
    cram = diff_bytes(rc, mc, "CRAM")
    verdict = ("MATCH (byte-identical VRAM+CRAM)"
               if vram["differing"] == 0 and cram["differing"] == 0
               else "DIVERGENT")
    res = {"vram": vram, "cram": cram, "verdict": verdict}

    for d in (vram, cram):
        print(f"\n  {d['label']}: {d['compared']} bytes compared, "
              f"{d['differing']} differ ({d['match_pct']}% match)")
        if d["first_divergence"]:
            print(f"    first divergence @ {d['first_divergence']} ({d['first_region']})")
            for s in d["samples"]:
                print(f"      {s['off_hex']}: recomp={s['recomp']:3d} mesen={s['mesen']:3d}  [{s['region']}]")
    print(f"\n  VERDICT: {verdict}\n")
    if a.json:
        json.dump(res, open(a.json, "w"), indent=2)
        print(f"  wrote {a.json}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
