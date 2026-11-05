#!/usr/bin/env python3
"""
plot_nilm.py -- the chart that makes the Phase 5 claim checkable.

Reads the CSV from the NILM scorer and draws, on a shared time axis:

  1. the aggregate power -- THE ONLY INPUT the classifier is given,
  2. per-load truth against what the classifier inferred from it,
  3. the residual, with the moments it declined to answer shaded.

The point of the layout is that panel 1 is one wiggly line and panel 2 is
three clean on/off traces recovered from it. If the inferred traces sit on top
of the truth bands, the disaggregation worked.

    pio run -e native-nilm -d meter_node
    ./meter_node/.pio/build/native-nilm/program --csv > docs/captures/nilm_score.csv
    python tools/plot_nilm.py docs/captures/nilm_score.csv -o docs/captures/nilm_score.png
"""

import argparse
import csv
import sys

LOADS = ["base", "charger", "thermal"]


def parse_mask(s):
    """'base+thermal' -> {'base', 'thermal'}; 'off' -> set()."""
    s = (s or "").strip()
    if not s or s == "off":
        return set()
    return set(s.split("+"))


def load(path):
    rows = []
    with open(path, newline="") as fh:
        for r in csv.DictReader(fh):
            rows.append({
                "t": int(r["t_ms"]) / 1000.0,
                "p": float(r["power_w"]),
                "truth": parse_mask(r["truth"]),
                "guess": parse_mask(r["inferred"]),
                "conf": r["confident"] == "1",
                "resid": float(r["residual_w"]),
            })
    return rows


def spans(rows, pred):
    """Contiguous [start, end] time spans where pred(row) holds."""
    out, start = [], None
    for r in rows:
        if pred(r) and start is None:
            start = r["t"]
        elif not pred(r) and start is not None:
            out.append((start, r["t"]))
            start = None
    if start is not None:
        out.append((start, rows[-1]["t"]))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("csvfile")
    ap.add_argument("-o", "--out")
    ap.add_argument("--seconds", type=float, default=120.0,
                    help="how much to draw; the default is two simulator cycles")
    args = ap.parse_args()

    rows = load(args.csvfile)
    if not rows:
        sys.exit("csv is empty")
    rows = [r for r in rows if r["t"] <= args.seconds]

    try:
        import matplotlib
        if args.out:
            matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        from matplotlib.patches import Patch
    except ImportError:
        sys.exit("matplotlib not installed.  pip install matplotlib")

    t = [r["t"] for r in rows]

    fig, ax = plt.subplots(3, 1, sharex=True, figsize=(11, 9),
                           gridspec_kw={"height_ratios": [2, 2.4, 1]})

    # --- 1: the single signal the classifier is allowed to see -------------
    ax[0].plot(t, [r["p"] for r in rows], lw=0.9, color="#2c3e50")
    ax[0].set_ylabel("Aggregate power (W)")
    ax[0].set_title("One meter, three loads: disaggregation against ground truth")
    ax[0].annotate("this is the only input", xy=(0.015, 0.86),
                   xycoords="axes fraction", fontsize=9, color="#555555")
    ax[0].grid(alpha=0.3)

    # --- 2: truth as a band, the inferred answer drawn on top -------------
    colours = {"base": "#16a085", "charger": "#c0392b", "thermal": "#8e44ad"}
    for k, name in enumerate(LOADS):
        y = len(LOADS) - 1 - k           # base at the top, reading downward
        for a, b in spans(rows, lambda r, nm=name: nm in r["truth"]):
            ax[1].axhspan(y - 0.30, y + 0.30, xmin=0, xmax=1, alpha=0)
            ax[1].fill_between([a, b], y - 0.30, y + 0.30,
                               color=colours[name], alpha=0.22, linewidth=0)
        # The inferred trace: a step line that should ride the band's edges.
        ax[1].step(t, [y + (0.30 if name in r["guess"] else -0.30) for r in rows],
                   where="post", lw=1.5, color=colours[name])
        # Mark any sample where a committed answer was actually wrong.
        wrong = [r["t"] for r in rows
                 if r["conf"] and (name in r["truth"]) != (name in r["guess"])]
        if wrong:
            ax[1].plot(wrong, [y] * len(wrong), "x", ms=7, mew=1.8,
                       color="#000000", zorder=5)

    ax[1].set_yticks(range(len(LOADS)))
    ax[1].set_yticklabels(list(reversed(LOADS)))
    ax[1].set_ylim(-0.75, len(LOADS) - 0.25)
    ax[1].set_ylabel("Load state")
    ax[1].legend(handles=[
        Patch(facecolor="#7f8c8d", alpha=0.22, label="truth (simulator label)"),
        Patch(facecolor="none", edgecolor="#2c3e50", label="inferred from power"),
    ], loc="lower right", fontsize=8.5, framealpha=0.9)
    ax[1].grid(alpha=0.2, axis="x")

    # --- 3: residual, and where it declined to answer ---------------------
    ax[2].plot(t, [r["resid"] for r in rows], lw=1.0, color="#2980b9")
    ax[2].axhline(0, color="#7f8c8d", lw=0.8)
    for a, b in spans(rows, lambda r: not r["conf"]):
        ax[2].axvspan(a, b, color="#e67e22", alpha=0.25, linewidth=0)
        ax[1].axvspan(a, b, color="#e67e22", alpha=0.12, linewidth=0)
    ax[2].set_ylabel("Residual (W)")
    ax[2].set_xlabel("time (s)")
    ax[2].annotate("shaded: declined to answer (mid-transition) -- "
                   "being late beats being confidently wrong",
                   xy=(0.015, 0.14), xycoords="axes fraction",
                   fontsize=8.5, color="#555555")
    ax[2].grid(alpha=0.3)

    committed = sum(1 for r in rows if r["conf"])
    exact = sum(1 for r in rows if r["conf"] and r["truth"] == r["guess"])
    print(f"drawn {len(rows)} samples; committed {committed}; "
          f"exact {exact} ({100.0 * exact / max(committed, 1):.2f}%)")

    fig.tight_layout()
    if args.out:
        fig.savefig(args.out, dpi=130)
        print(f"written to {args.out}")
    else:
        plt.show()
    return 0


if __name__ == "__main__":
    sys.exit(main())
