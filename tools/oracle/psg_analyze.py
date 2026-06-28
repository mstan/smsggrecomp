#!/usr/bin/env python3
"""Analyze a recomp PSG chip_ring (cyc,val) and reconstruct SN76489 register
state over time. Primary question (Axis-5 Risk #1): does the game ever write a
TONE PERIOD of 0? If not, our period-0 clamp-to-1 (vs real HW 0x400) is moot.

Usage: python psg_analyze.py psg_ring.csv
"""
import sys
from collections import Counter


def main():
    rows = [l.strip().split(",") for l in open(sys.argv[1]) if l.strip() and not l.startswith("cyc")]
    period = [1, 1, 1]          # 10-bit tone reload, channels 0..2
    vol = [15, 15, 15, 15]
    latch_ch, latch_type = 0, 0
    period0_events = []         # (cyc, channel) where a tone period becomes 0
    period_hist = Counter()
    writes = 0
    for cyc, val in rows:
        cyc = int(cyc); b = int(val) & 0xFF; writes += 1
        if b & 0x80:                       # LATCH/DATA
            latch_ch = (b >> 5) & 3
            latch_type = (b >> 4) & 1
            if latch_type == 1:            # volume
                vol[latch_ch] = b & 0xF
            elif latch_ch < 3:             # tone low nibble
                period[latch_ch] = (period[latch_ch] & 0x3F0) | (b & 0xF)
                period_hist[period[latch_ch]] += 1
                if period[latch_ch] == 0:
                    period0_events.append((cyc, latch_ch))
        else:                              # DATA (high 6 bits)
            if latch_type == 0 and latch_ch < 3:
                period[latch_ch] = (period[latch_ch] & 0xF) | ((b & 0x3F) << 4)
                period_hist[period[latch_ch]] += 1
                if period[latch_ch] == 0:
                    period0_events.append((cyc, latch_ch))

    print(f"  PSG writes: {writes}")
    print(f"  distinct tone periods seen: {len(period_hist)}")
    nz = [p for p in period_hist if p > 0]
    print(f"  min nonzero period: {min(nz) if nz else None}  max: {max(period_hist) if period_hist else None}")
    print(f"  PERIOD-0 events: {len(period0_events)}")
    if period0_events:
        for cyc, ch in period0_events[:10]:
            print(f"    cyc={cyc} ch={ch}")
        print("  => period-0 IS written: our clamp-to-1 (HW=0x400) is a REAL divergence (Risk #1 LIVE)")
    else:
        print("  => period-0 NEVER written: the clamp-to-1 vs 0x400 gap is MOOT for this title")
    # lowest periods (highest pitch) — where clamp behavior matters most
    print("  lowest 5 periods (count): " +
          ", ".join(f"{p}:{period_hist[p]}" for p in sorted(period_hist)[:5]))


if __name__ == "__main__":
    sys.exit(main())
