#!/usr/bin/env python3
"""Throwaway plots for a dariyanaap run. Not a product.

    python3 tools/plot.py runs/e2          # whatever is in the directory
    python3 tools/plot.py runs/e2 --show

Draws whichever of the three files it finds:

  summary.csv     throughput and p99 against connections — the E2 shape
  histogram.csv   the latency CDF, from raw slot counts
  timeseries.csv  p99 per second — when it degraded, not just by how much

The CDF is drawn from slot counts rather than from the percentile columns,
which is the point of keeping them (DESIGN.md decision 4): the file alone is
enough, months later, without this binary.
"""
import csv
import sys
from pathlib import Path

try:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
except ImportError:
    sys.exit("needs matplotlib: pip install matplotlib")


def read(path):
    with open(path) as handle:
        return list(csv.DictReader(handle))


def plot_summary(rows, axis):
    # One line per mode, because a closed-loop row and an open-loop row are
    # not comparable points on one curve (E3).
    for mode in sorted({r.get("mode", "closed-loop") for r in rows}):
        pick = [r for r in rows if r.get("mode", "closed-loop") == mode]
        pick.sort(key=lambda r: int(r["connections_requested"]))
        conns = [int(r["connections_requested"]) for r in pick]
        rps = [float(r["per_second"]) for r in pick]
        axis.plot(conns, rps, marker="o", label=f"{mode} throughput")
        short = [r for r in pick
                 if int(r["connections_started"]) < int(r["connections_requested"])]
        if short:
            # A row that offered less load than it claims is not comparable to
            # the rows above it, so it is marked rather than silently plotted.
            axis.scatter([int(r["connections_requested"]) for r in short],
                         [float(r["per_second"]) for r in short],
                         marker="x", s=120, zorder=5,
                         label=f"{mode}: rig short of connections")
    axis.set_xscale("log", base=2)
    axis.set_xlabel("connections")
    axis.set_ylabel("requests/second")
    axis.set_title("throughput vs concurrency")
    axis.grid(alpha=0.3)
    axis.legend(fontsize=8)


def plot_cdf(rows, axis):
    slots = [(int(r["high_ns"]), int(r["count"])) for r in rows if int(r["slot"]) >= 0]
    slots.sort()
    total = sum(count for _, count in slots)
    if total == 0:
        return
    running = 0
    xs, ys = [], []
    for high, count in slots:
        running += count
        xs.append(high / 1000.0)
        ys.append(100.0 * running / total)
    axis.plot(xs, ys)
    axis.set_xscale("log")
    axis.set_xlabel("latency (us, log)")
    axis.set_ylabel("percentile")
    axis.set_title(f"latency CDF ({total:,} samples)")
    # The percentiles worth reading off. A linear y-axis hides the tail, which
    # is the only part this repo cares about.
    for mark in (50, 90, 99, 99.9):
        axis.axhline(mark, color="grey", lw=0.5, ls=":")
    axis.set_ylim(0, 100)
    axis.grid(alpha=0.3)


def plot_timeseries(rows, axis):
    seconds = [int(r["second"]) for r in rows]
    p99 = [int(r["p99_ns"]) / 1000.0 for r in rows]
    counts = [int(r["count"]) for r in rows]
    axis.plot(seconds, p99, marker="o", label="p99 (us)")
    # Seconds with no samples are the most informative points in the file: the
    # target answered nothing at all.
    empty = [s for s, c in zip(seconds, counts) if c == 0]
    for second in empty:
        axis.axvspan(second - 0.5, second + 0.5, color="red", alpha=0.15)
    if empty:
        axis.plot([], [], color="red", alpha=0.3, lw=8, label="no samples")
    axis.set_xlabel("second")
    axis.set_ylabel("p99 (us)")
    axis.set_title("p99 per second")
    axis.grid(alpha=0.3)
    axis.legend(fontsize=8)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    directory = Path(sys.argv[1])
    panels = []
    for name, drawer in (("summary.csv", plot_summary),
                         ("histogram.csv", plot_cdf),
                         ("timeseries.csv", plot_timeseries)):
        path = directory / name
        if path.exists():
            panels.append((name, drawer, read(path)))
    if not panels:
        sys.exit(f"no summary.csv, histogram.csv or timeseries.csv in {directory}")

    figure, axes = plt.subplots(1, len(panels), figsize=(6 * len(panels), 4.5))
    if len(panels) == 1:
        axes = [axes]
    for axis, (name, drawer, rows) in zip(axes, panels):
        drawer(rows, axis)
    figure.tight_layout()
    out = directory / "plot.png"
    figure.savefig(out, dpi=120)
    print(f"wrote {out}")
    if "--show" in sys.argv:
        plt.show()


if __name__ == "__main__":
    main()
