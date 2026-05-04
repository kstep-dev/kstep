#!/usr/bin/env python3

import argparse
import re
from collections import defaultdict

import matplotlib.pyplot as plt
import numpy as np
from consts import RESULTS_DIR

# Parse both nr_running and nr_queued from a caseSensitiveTime.log file.
# Plot both, for CPUs 4,5,6,7, on the same figure.

def extract_running_queued(filename, cpus=None, time_start=0.0):
    """
    Returns:
        - cpu_list: list of CPUs in order
        - timestamps: sorted list of float timestamps
        - nr_running: matrix [cpu][t] values (np.nan if not present)
        - nr_queued: matrix [cpu][t] (np.nan if not present)
        - min_running, max_running
        - min_queued, max_queued
    """
    if cpus is None:
        cpus = [4,5,6,7]
    cpu_indices = {cpu: idx for idx, cpu in enumerate(cpus)}

    pat = re.compile(
        r"\[\s*(\d+\.\d+)\].*?print_rq_stats: - CPU (\d+) running=(\d+), queued=(\d+),"
    )

    timeset = set()
    vals_running = defaultdict(dict)  # cpu -> ts -> value
    vals_queued = defaultdict(dict)
    min_running, max_running = float('inf'), float('-inf')
    min_queued, max_queued = float('inf'), float('-inf')

    with open(filename, "r") as f:
        for line in f:
            m = pat.search(line)
            if m:
                ts = float(m.group(1))
                if ts < time_start:
                    continue
                cpu = int(m.group(2))
                if cpu not in cpus:
                    continue
                running = int(m.group(3))
                queued = int(m.group(4))
                timeset.add(ts)
                vals_running[cpu][ts] = running
                vals_queued[cpu][ts] = queued
                min_running = min(min_running, running)
                max_running = max(max_running, running)
                min_queued = min(min_queued, queued)
                max_queued = max(max_queued, queued)

    timestamps = sorted(timeset)
    nc = len(cpus)
    nt = len(timestamps)

    nr_running = np.full((nc, nt), np.nan)
    nr_queued = np.full((nc, nt), np.nan)
    for cpu in cpus:
        for j, ts in enumerate(timestamps):
            if ts in vals_running[cpu]:
                nr_running[cpu_indices[cpu], j] = vals_running[cpu][ts]
            if ts in vals_queued[cpu]:
                nr_queued[cpu_indices[cpu], j] = vals_queued[cpu][ts]

    return cpus, timestamps, nr_running, nr_queued, int(min_running), int(max_running), int(min_queued), int(max_queued)

def plot_running_queued(
    cpus, timestamps, nr_running, nr_queued,
    min_running, max_running, min_queued, max_queued,
    out_pdf, title="CPUs 4-7 Running and Queued"
):
    # X: ms offset from first timestamp
    t0 = timestamps[0]
    x_ms = [(t-t0)*1000 for t in timestamps]

    fig, ax = plt.subplots(figsize=(5, 2))

    colors_running = ['#2166AC', '#4393C3', '#92C5DE', '#D6604D']
    colors_queued =  ['#B2182B', '#D6604D', '#FDB863', '#FCC1A8']
    labels = [f"CPU {cpu}" for cpu in cpus]

    # --------- Highlight regions where nr_queued > nr_running with a more obvious shade ---------
    # For each CPU, we find contiguous segments where nr_queued > nr_running
    highlight_alpha = 0.45  # Make the shade more opaque
    highlight_color = "#A3485A"  # Brighter yellow for more obvious highlight

    y_min = 0
    y_max = max(max_running, max_queued) + 1

    for idx, cpu in enumerate(cpus):
        r = nr_running[idx, :]
        q = nr_queued[idx, :]
        valid_mask = (~np.isnan(r)) & (~np.isnan(q))
        where_qgt = np.zeros_like(x_ms, dtype=bool)
        where_qgt[valid_mask] = q[valid_mask] > r[valid_mask]
        # We want to shade horizontally along x_ms wherever condition holds
        # Find contiguous blocks
        inside = False
        start_idx = None
        for i in range(len(x_ms)):
            if where_qgt[i] and not inside:
                inside = True
                start_idx = i
            if (not where_qgt[i] or i == len(x_ms)-1) and inside:
                inside = False
                end_idx = i if not where_qgt[i] else i+1
                x_start = x_ms[start_idx]
                x_end = x_ms[end_idx-1] if end_idx-1 < len(x_ms) else x_ms[-1]
                # Shade region: always the full y range of this CPU's row, from lower y to higher y
                row_height = 1.0 / len(cpus)
                yrowmin = y_min + row_height * idx * (y_max - y_min)
                yrowmax = y_min + row_height * (idx + 1) * (y_max - y_min)
                # Use ax.fill_between for vertical shading from yrowmin to yrowmax
                ax.fill_between(
                    [x_start, x_end + 1],
                    [yrowmin, yrowmin],
                    [yrowmax, yrowmax],
                    color=highlight_color,
                    alpha=highlight_alpha,
                    zorder=0,
                    label=None if start_idx != 0 else 'nr_queued > nr_running'
                )
    # -----------------------------------------------------------------------------

    # Plot nr_running and nr_queued for each CPU using step plot
    for idx, cpu in enumerate(cpus):
        # Remove nan (mask for step plotting)
        mask_running = ~np.isnan(nr_running[idx, :])
        mask_queued = ~np.isnan(nr_queued[idx, :])
        # Only plot if at least one data point exists
        if np.any(mask_running):
            ax.step(
                np.array(x_ms)[mask_running], nr_running[idx, :][mask_running],
                where='post',
                label=f"runnable tasks",
                markersize=2, lw=2, linestyle='-'
            )
        if np.any(mask_queued):
            ax.step(
                np.array(x_ms)[mask_queued], nr_queued[idx, :][mask_queued],
                where='post',
                label=f"queued tasks",
                markersize=2, lw=2, linestyle='--'
            )

    # Y-axis: auto span both nr_running/nr_queued min/max
    ax.set_ylim(0, y_max)
    ax.set_xlim(0, max(x_ms))

    ax.set_ylabel("Number of Tasks")
    ax.grid(True, alpha=0.3)

    # Custom legend: Add extra label for shading
    handles, labels_ = ax.get_legend_handles_labels()
    import matplotlib.patches as mpatches
    # Add only one patch for the shade if not present
    if any(isinstance(h, mpatches.Patch) and h.get_label() == 'nr_queued > nr_running' for h in handles):
        pass
    else:
        patch = mpatches.Patch(color=highlight_color, alpha=highlight_alpha, label='queue tasks > runnable tasks')
        handles = [patch] + handles
        labels_ = ['queue tasks > runnable tasks'] + labels_
    ax.legend(handles, labels_, loc='lower right')

    # xticks at every 50ms: 50, 100, 150, ...
    max_x = max(x_ms)
    xtick_spacing = 50
    xticks = list(range(
        xtick_spacing,
        int(max_x) + xtick_spacing,
        xtick_spacing
    ))
    # Clip to available data range for x_ms
    xticks = [x for x in xticks if x <= x_ms[-1]]
    # At least the first x_ms value if no ticks in range
    if len(xticks) == 0:
        xticks = [x_ms[0]]
    ax.set_xticks(xticks)
    ax.set_xticklabels([f"{int(x)}" for x in xticks])
    ax.set_xlabel("Time offset (ms)")
    ax.set_yticks(np.arange(0, y_max + 1))

    plt.tight_layout()
    plt.savefig(out_pdf, bbox_inches=None, pad_inches=0.23)
    print("Saved plot to", out_pdf)

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--log", type=str, default=None, help="Path to @caseSensitiveTime.log or similar log")
    parser.add_argument("--cpus", type=str, default="2", help="Comma-separated CPUs to plot (default 1)")
    parser.add_argument("--time-start", type=float, default=10, help="Only plot data from this time onward (in seconds)")
    parser.add_argument("--output", type=str, default=None, help="PDF file for plot output")
    args = parser.parse_args()

    # Default log file: data/logs/caseSensitiveTime.log
    if args.log is None:
        log_path = RESULTS_DIR / "caseSensitiveTime.log"
    else:
        log_path = args.log

    cpus = [int(x) for x in args.cpus.split(',')]
    pdf_fn = args.output if args.output else f"{RESULTS_DIR}/nr_running_queued.pdf"

    cpus, timestamps, nr_running, nr_queued, min_running, max_running, min_queued, max_queued = extract_running_queued(
        log_path, cpus=cpus, time_start=args.time_start
    )

    plot_title = f"CPUs {', '.join(str(c) for c in cpus)}: nr_running and nr_queued"
    plot_running_queued(
        cpus, timestamps, nr_running, nr_queued,
        min_running, max_running, min_queued, max_queued,
        out_pdf=pdf_fn,
        title=plot_title,
    )
