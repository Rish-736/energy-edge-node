#!/usr/bin/env python3
"""
replay.py -- read a meter_node telemetry capture, check it, and plot it.

The point of this tool is to prove the log format round-trips. If a capture
taken today can be parsed, validated and plotted by a separate program, then
it is a real dataset -- which is what Phase 5 (NILM) will train against.

Usage
    python tools/replay.py capture.csv
    python tools/replay.py capture.csv --no-plot      # just the summary
    python tools/replay.py capture.csv -o out.png     # save instead of show

Capture a log with:
    pio device monitor -e esp32dev-sim | tee capture.csv
"""

import argparse
import sys

FIELDS = ["seq", "t_ms", "v", "i", "p", "wh", "flags", "truth"]

FLAG_SENSOR_FAIL = 0x01
FLAG_SIMULATED   = 0x80

LOADS = [(0x01, "base"), (0x02, "charger"), (0x04, "thermal")]


def truth_str(mask):
    names = [n for bit, n in LOADS if mask & bit]
    return "+".join(names) if names else "off"


def parse(path):
    """Pull 'T,' rows out of a serial capture, ignoring boot noise and notes."""
    rows, notes, skipped = [], [], 0

    with open(path, "r", errors="replace") as fh:
        for line in fh:
            line = line.strip()
            if line.startswith("#"):
                notes.append(line)
                continue
            if not line.startswith("T,"):
                continue

            parts = line.split(",")
            if len(parts) != len(FIELDS) + 1:
                skipped += 1
                continue
            try:
                rows.append({
                    "seq":   int(parts[1]),
                    "t_ms":  int(parts[2]),
                    "v":     float(parts[3]),
                    "i":     float(parts[4]),
                    "p":     float(parts[5]),
                    "wh":    float(parts[6]),
                    "flags": int(parts[7]),
                    "truth": int(parts[8]),
                })
            except ValueError:
                skipped += 1

    return rows, notes, skipped


def summarise(rows, notes, skipped):
    if not rows:
        print("No telemetry rows found.", file=sys.stderr)
        return False

    for n in notes:
        print(n)

    span_s = (rows[-1]["t_ms"] - rows[0]["t_ms"]) / 1000.0
    powers = [r["p"] for r in rows]

    # A gap in seq means the logger was lapped -- the firmware counts these,
    # and we independently confirm it here from the data itself.
    gaps = sum(rows[k]["seq"] - rows[k - 1]["seq"] - 1 for k in range(1, len(rows)))
    faults = sum(1 for r in rows if r["flags"] & FLAG_SENSOR_FAIL)

    print(f"\nsamples      {len(rows)}")
    print(f"duration     {span_s:.1f} s")
    if span_s > 0:
        print(f"rate         {len(rows) / span_s:.2f} Hz")
    print(f"dropped      {gaps}")
    print(f"sensor fail  {faults}")
    print(f"malformed    {skipped}")
    print(f"simulated    {bool(rows[0]['flags'] & FLAG_SIMULATED)}")
    print(f"\nvoltage      {min(r['v'] for r in rows):.2f} .. "
          f"{max(r['v'] for r in rows):.2f} V")
    print(f"power        {min(powers):.2f} .. {max(powers):.2f} W  "
          f"(mean {sum(powers) / len(powers):.2f})")
    print(f"energy       {rows[-1]['wh']:.4f} Wh")

    # Peak vs mean is the number Phase 3 exists to shrink. Printing it now
    # gives the "before" figure to compare the demand-response demo against.
    mean_p = sum(powers) / len(powers)
    if mean_p > 0:
        print(f"peak/mean    {max(powers) / mean_p:.2f}x   <-- Phase 3 target")

    if any(r["truth"] for r in rows):
        print("\nground truth (simulator):")
        seen = {}
        for r in rows:
            seen[r["truth"]] = seen.get(r["truth"], 0) + 1
        for mask, count in sorted(seen.items(), key=lambda kv: -kv[1]):
            print(f"  {truth_str(mask):<22} {count:5d} samples")

    return True


def plot(rows, out):
    try:
        import matplotlib.pyplot as plt
    except ImportError:
        print("\n(matplotlib not installed -- skipping plot. "
              "`pip install matplotlib` to enable.)", file=sys.stderr)
        return

    t  = [r["t_ms"] / 1000.0 for r in rows]
    t0 = t[0]
    t  = [x - t0 for x in t]

    fig, ax = plt.subplots(4, 1, sharex=True, figsize=(11, 9))

    ax[0].plot(t, [r["p"] for r in rows], lw=1.0, color="#c0392b")
    ax[0].set_ylabel("Power (W)")
    ax[0].set_title("Energy Edge Node -- telemetry capture")

    ax[1].plot(t, [r["v"] for r in rows], lw=1.0, color="#2980b9")
    ax[1].set_ylabel("Bus (V)")

    ax[2].plot(t, [r["i"] for r in rows], lw=1.0, color="#27ae60")
    ax[2].set_ylabel("Current (A)")

    ax[3].plot(t, [r["wh"] for r in rows], lw=1.2, color="#8e44ad")
    ax[3].set_ylabel("Energy (Wh)")
    ax[3].set_xlabel("time (s)")

    # Shade which appliances were really on, so the transients in the power
    # trace line up visibly with their cause.
    if any(r["truth"] for r in rows):
        colors = {0x01: "#f1c40f", 0x02: "#e67e22", 0x04: "#16a085"}
        for bit, color in colors.items():
            on = None
            for k, r in enumerate(rows):
                active = bool(r["truth"] & bit)
                if active and on is None:
                    on = t[k]
                elif not active and on is not None:
                    ax[0].axvspan(on, t[k], color=color, alpha=0.15)
                    on = None
            if on is not None:
                ax[0].axvspan(on, t[-1], color=color, alpha=0.15)

    for a in ax:
        a.grid(alpha=0.3)

    fig.tight_layout()
    if out:
        fig.savefig(out, dpi=130)
        print(f"\nplot written to {out}")
    else:
        plt.show()


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("logfile")
    ap.add_argument("-o", "--out", help="save the plot to this file")
    ap.add_argument("--no-plot", action="store_true")
    args = ap.parse_args()

    rows, notes, skipped = parse(args.logfile)
    if not summarise(rows, notes, skipped):
        return 1
    if not args.no_plot:
        plot(rows, args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
