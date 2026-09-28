#!/usr/bin/env python3
"""Plot the CSVs produced by run_benchmarks.sh.

    pip3 install matplotlib
    python3 scripts/plot.py results/<hostname>

Writes PNG figures next to the CSVs, ready to drop into the report.
Only the standard library + matplotlib are needed.
"""
import csv
import sys
from collections import defaultdict
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
from matplotlib.ticker import EngFormatter  # noqa: E402

# Fixed categorical order: an implementation keeps the same color and marker
# in every figure. Markers give a second encoding for print / colour-blindness.
PALETTE = ["#2a78d6", "#eb6834", "#1baf7a", "#eda100",
           "#e87ba4", "#008300", "#4a3aa7", "#e34948"]
MARKERS = ["o", "s", "^", "D", "v", "P", "X", "*"]

ORDER = [
    # locks
    "tas", "tas_backoff", "ttas", "ttas_backoff", "mcs",
    "std_mutex", "pthread_mutex", "omp_lock",
    # barriers
    "sense", "sense_backoff", "std_barrier", "pthread_cond", "omp_barrier",
    # counters
    "lock_tas", "lock_ttas", "lock_ttas_backoff", "lock_mcs",
    "lock_std_mutex", "cas", "cas_backoff", "faa",
]


def style_for(names):
    """Assign colors in fixed order over the series of ONE figure."""
    ordered = [n for n in ORDER if n in names] + sorted(set(names) - set(ORDER))
    return {n: (PALETTE[i % len(PALETTE)], MARKERS[i % len(MARKERS)])
            for i, n in enumerate(ordered)}, ordered


def read(path):
    with open(path) as f:
        return list(csv.DictReader(f))


def setup_axes(ax, title, ylabel, logy=False):
    ax.set_title(title, fontsize=10, loc="left")
    ax.set_xlabel("threads")
    ax.set_ylabel(ylabel)
    if logy:
        ax.set_yscale("log")
    else:
        ax.yaxis.set_major_formatter(EngFormatter(sep=""))  # 800k, 1.1M
    ax.grid(True, color="#e5e5e3", linewidth=0.8)
    ax.set_axisbelow(True)
    for side in ("top", "right"):
        ax.spines[side].set_visible(False)
    for side in ("left", "bottom"):
        ax.spines[side].set_color("#8a8984")


def plot_panels(panels, key, x, y, ylabel, suptitle, out, logy=False):
    """panels: list of (title, rows). One subplot per panel, shared legend."""
    names = sorted({r[key] for _, rows in panels for r in rows})
    styles, ordered = style_for(names)
    cols = min(len(panels), 2)
    nrows = (len(panels) + cols - 1) // cols
    fig, axes = plt.subplots(nrows, cols, figsize=(6 * cols, 4 * nrows), squeeze=False)
    for ax, (title, rows) in zip(axes.flat, panels):
        series = defaultdict(list)
        for r in rows:
            series[r[key]].append((int(r[x]), float(r[y])))
        for name in ordered:
            if name not in series:
                continue
            pts = sorted(series[name])
            color, marker = styles[name]
            ax.plot([p[0] for p in pts], [p[1] for p in pts], color=color,
                    marker=marker, markersize=6, linewidth=2, label=name)
        setup_axes(ax, title, ylabel, logy)
        xs = sorted({int(r[x]) for r in rows})
        ax.set_xticks(xs)
    for ax in list(axes.flat)[len(panels):]:
        ax.set_visible(False)
    handles, labels = axes.flat[0].get_legend_handles_labels()
    for ax in list(axes.flat)[1:len(panels)]:
        for h, l in zip(*ax.get_legend_handles_labels()):
            if l not in labels:
                handles.append(h)
                labels.append(l)
    fig.legend(handles, labels, loc="lower center", ncol=min(len(labels), 4),
               frameon=False, bbox_to_anchor=(0.5, -0.01))
    fig.suptitle(suptitle, x=0.01, ha="left", fontsize=12)
    fig.tight_layout(rect=(0, 0.06 + 0.02 * ((len(labels) - 1) // 4), 1, 1))
    fig.savefig(out, dpi=160, bbox_inches="tight")
    plt.close(fig)
    print("wrote", out)


def main(d):
    d = Path(d)
    ncpu = None
    machine = d / "machine.txt"
    if machine.exists():
        for line in machine.read_text().splitlines():
            if line.startswith("logical cpus:"):
                ncpu = int(line.split(":")[1])

    def in_range(r):  # oversubscribed points go in their own figure
        return ncpu is None or int(r["threads"]) <= ncpu

    # ---- Part 1: locks ------------------------------------------------------
    if (d / "locks.csv").exists():
        rows = read(d / "locks.csv")
        spin = [r for r in rows if r["backoff"] == "spin" and in_range(r)]
        configs = sorted({(int(r["n"]), int(r["outside_work"])) for r in spin},
                         key=lambda c: (-c[0], c[1]))
        panels = [(f"N={n}, OUTSIDE_WORK={w}  ({100 // (w + 1)}% of work in CS)",
                   [r for r in spin if int(r["n"]) == n and int(r["outside_work"]) == w])
                  for n, w in configs]
        plot_panels(panels, "lock", "threads", "cs_per_sec", "critical sections / s",
                    "Part 1 - lock throughput vs threads (higher is better)",
                    d / "locks_throughput.png")
        plot_panels([(f"N={n}, OUTSIDE_WORK={w}",
                      [r for r in spin if int(r["n"]) == n and int(r["outside_work"]) == w])
                     for n, w in configs],
                    "lock", "threads", "time_s", "execution time (s)",
                    "Part 1 - execution time vs threads (lower is better)",
                    d / "locks_time.png")
        backoff = [r for r in rows if r["lock"] in ("tas_backoff", "ttas_backoff")
                   and int(r["n"]) == 2003 and int(r["outside_work"]) == 0 and in_range(r)]
        for r in backoff:
            r["variant"] = f'{r["lock"]} ({r["backoff"]})'
        if backoff:
            plot_panels([("backoff: busy-wait vs sleep_for, N=2003, OUTSIDE_WORK=0", backoff)],
                        "variant", "threads", "cs_per_sec", "critical sections / s",
                        "Part 1 - effect of the backoff strategy",
                        d / "locks_backoff.png")

    # ---- Part 2: barriers ---------------------------------------------------
    if (d / "barriers.csv").exists():
        rows = [r for r in read(d / "barriers.csv") if int(r["threads"]) > 1]
        for r in rows:
            r["variant"] = r["barrier"] + (" (sleep)" if r["backoff"] == "sleep"
                                           and r["barrier"] == "sense_backoff" else "")
        plot_panels([("no work between barriers", rows)], "variant", "threads",
                    "episodes_per_sec", "barrier episodes / s (log)",
                    "Part 2 - barrier throughput vs threads (higher is better)",
                    d / "barriers.png", logy=True)

    # ---- Part 3: counters ---------------------------------------------------
    if (d / "counters.csv").exists():
        rows = read(d / "counters.csv")
        works = sorted({int(r["work"]) for r in rows})
        plot_panels([(f"work between increments = {w}",
                      [r for r in rows if int(r["work"]) == w]) for w in works],
                    "counter", "threads", "mops_per_sec", "million increments / s (log)",
                    "Part 3 - counter throughput vs threads (higher is better)",
                    d / "counters.png", logy=True)
        plot_panels([(f"work between increments = {w}",
                      [r for r in rows if int(r["work"]) == w]) for w in works],
                    "counter", "threads", "time_s", "execution time (s, log)",
                    "Part 3 - counter execution time vs threads (lower is better)",
                    d / "counters_time.png", logy=True)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(sys.argv[1])
