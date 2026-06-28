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
    period0_events = []         # (cyc, channel, vol) where a tone period becomes 0
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
                    period0_events.append((cyc, latch_ch, vol[latch_ch]))
        else:                              # DATA (high 6 bits)
            if latch_type == 0 and latch_ch < 3:
                period[latch_ch] = (period[latch_ch] & 0xF) | ((b & 0x3F) << 4)
                period_hist[period[latch_ch]] += 1
                if period[latch_ch] == 0:
                    period0_events.append((cyc, latch_ch, vol[latch_ch]))

    print(f"  PSG writes: {writes}")
    print(f"  distinct tone periods seen: {len(period_hist)}")
    nz = [p for p in period_hist if p > 0]
    print(f"  min nonzero period: {min(nz) if nz else None}  max: {max(period_hist) if period_hist else None}")
    print(f"  PERIOD-0 events: {len(period0_events)}")
    if period0_events:
        # vol 15 = silent; <15 = audible. The clamp bug only matters at audible volume.
        audible = [e for e in period0_events if e[2] < 15]
        print(f"    of which at AUDIBLE volume (<15): {len(audible)}")
        for cyc, ch, v in period0_events[:6]:
            print(f"    cyc={cyc} ch={ch} vol={v}{' (AUDIBLE)' if v<15 else ' (silent)'}")
        # NOTE: the correct period-0 reload is PLATFORM-dependent. GPGX
        # (sound.c) uses PSG_INTEGRATED (period 0 -> 0x1 == clamp-to-1) for SMS /
        # SMS2 / GG, and PSG_DISCRETE (0x400) ONLY for SG-1000. So for our SMS/GG
        # targets clamp-to-1 is CORRECT; 0x400 would be wrong.
        if audible:
            print("  => period-0 at AUDIBLE volume — exercises the reload value.")
            print("     SMS/GG integrated PSG => correct = 0x1 (== our clamp-to-1). Only SG-1000")
            print("     discrete uses 0x400. Confirm vs the GPGX audio diff (ALIGNED => clamp ok).")
        else:
            print("  => period-0 only while channel muted (vol=15): inaudible regardless of reload.")
    else:
        print("  => period-0 NEVER written: the clamp-to-1 vs 0x400 gap is MOOT for this title")
    # lowest periods (highest pitch) — where clamp behavior matters most
    print("  lowest 5 periods (count): " +
          ", ".join(f"{p}:{period_hist[p]}" for p in sorted(period_hist)[:5]))


if __name__ == "__main__":
    sys.exit(main())
