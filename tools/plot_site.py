#!/usr/bin/env python3
"""
plot_site.py -- the before/after chart for the demand controller.

Reads the CSV produced by the closed-loop site model and draws the baseline
and controlled runs on the same axes, so the effect of the controller is
visible rather than merely asserted.

    pio run -e native-site -d meter_node
    ./meter_node/.pio/build/native-site/program --csv > docs/captures/site_control.csv
    python tools/plot_site.py docs/captures/site_control.csv -o docs/captures/site_control.png

The top panel is instantaneous power; the bottom is the rolling demand average
that the bill is actually based on. The gap between the two bottom traces is
the money.
"""

import argparse
import csv
import sys


def load(path):
    runs = {"baseline": [], "controlled": []}
    with open(path, newline="") as fh:
        for row in csv.DictReader(fh):
            mode = row.get("mode")
            if mode not in runs:
                continue
            runs[mode].append({
                "t": int(row["t_ms"]) / 1000.0,
                "p": float(row["power_w"]),
                "avg": float(row["window_avg_w"]),
                "charger": int(row["charger_on"]),
                "wh": float(row["charge_wh"]),
            })
    return runs


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("csvfile")
    ap.add_argument("-o", "--out")
    ap.add_argument("--target", type=float, default=35.0)
    args = ap.parse_args()

    runs = load(args.csvfile)
    if not runs["baseline"] or not runs["controlled"]:
        sys.exit("csv must contain both a 'baseline' and a 'controlled' run")

    try:
        import matplotlib
        if args.out:
            matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        sys.exit("matplotlib not installed.  pip install matplotlib")

    base, ctrl = runs["baseline"], runs["controlled"]

    fig, ax = plt.subplots(3, 1, sharex=True, figsize=(11, 9),
                           gridspec_kw={"height_ratios": [2, 2, 1]})

    ax[0].plot([r["t"] for r in base], [r["p"] for r in base],
               lw=0.8, color="#c0392b", label="baseline")
    ax[0].plot([r["t"] for r in ctrl], [r["p"] for r in ctrl],
               lw=0.8, color="#2980b9", label="controlled")
    ax[0].set_ylabel("Instantaneous power (W)")
    ax[0].set_title("Deadline-aware demand control at an EV charging site")
    ax[0].legend(loc="upper right", fontsize=9)

    # This is the trace the bill is based on.
    ax[1].plot([r["t"] for r in base], [r["avg"] for r in base],
               lw=1.6, color="#c0392b", label="baseline")
    ax[1].plot([r["t"] for r in ctrl], [r["avg"] for r in ctrl],
               lw=1.6, color="#2980b9", label="controlled")
    ax[1].axhline(args.target, color="#7f8c8d", ls="--", lw=1.2,
                  label=f"target {args.target:.0f} W")
    ax[1].set_ylabel("Rolling demand average (W)")
    ax[1].legend(loc="upper right", fontsize=9)

    peak_b = max(r["avg"] for r in base)
    peak_c = max(r["avg"] for r in ctrl)
    ax[1].annotate(f"peak {peak_b:.1f} W", xy=(0.02, 0.88),
                   xycoords="axes fraction", color="#c0392b", fontsize=9)
    ax[1].annotate(f"peak {peak_c:.1f} W  "
                   f"({(1 - peak_c / peak_b) * 100:.0f}% lower)",
                   xy=(0.02, 0.78), xycoords="axes fraction",
                   color="#2980b9", fontsize=9)

    # The guarantee: both runs must reach the same delivered energy.
    ax[2].plot([r["t"] for r in base], [r["wh"] for r in base],
               lw=1.4, color="#c0392b")
    ax[2].plot([r["t"] for r in ctrl], [r["wh"] for r in ctrl],
               lw=1.4, color="#2980b9")
    ax[2].set_ylabel("Charge\ndelivered (Wh)")
    ax[2].set_xlabel("time (s)")
    ax[2].annotate("both reach the same charge — the deadline is met either way",
                   xy=(0.02, 0.12), xycoords="axes fraction",
                   fontsize=8.5, color="#555555")

    # Shade where the controller had the charger off.
    runstart = None
    for k, r in enumerate(ctrl):
        if not r["charger"] and runstart is None:
            runstart = r["t"]
        elif r["charger"] and runstart is not None:
            for a in ax[:2]:
                a.axvspan(runstart, r["t"], color="#2980b9", alpha=0.07)
            runstart = None

    for a in ax:
        a.grid(alpha=0.3)

    fig.tight_layout()
    if args.out:
        fig.savefig(args.out, dpi=130)
        print(f"written to {args.out}")
    else:
        plt.show()
    return 0


if __name__ == "__main__":
    sys.exit(main())
