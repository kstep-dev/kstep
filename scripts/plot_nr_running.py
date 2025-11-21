#!/usr/bin/env python3

import argparse
import re
from collections import defaultdict

import matplotlib.pyplot as plt
import numpy as np
from consts import LOGS_DIR, RESULTS_DIR
from matplotlib import colors

# Adapted to allow color-matrix style plot, like plot_cur_task.py.
# Only plot CPUs 4,5,6,7 -- CPUs 0,1,2,3 are not displayed or included
all_cpus = [4, 5, 6, 7]  # restrict to 4-7

# Use a discrete colormap (Blues) with N colors
N_COLORS = 7  # Show only (at most) 7 distinct colors for the colorbar/colormap
cmap = plt.cm.get_cmap('Blues', N_COLORS)

def build_nr_running_matrix(filename, time_start=0.0):
    """
    Builds a 2D matrix: rows for cpus, columns for sorted timestamps, values = nr_running
    Only collects values where timestamp >= time_start
    Only considers CPUs in all_cpus (e.g., 4,5,6,7) for plotting/matrix.
    """
    pattern_nr_running = re.compile(r"\[\s*(\d+\.\d+)\].*?print_nr_running:\s+(\d+)\s+(\d+)")
    pattern_print_tasks = re.compile(r"\[\s*(\d+\.\d+)\].*?print_tasks:.*?CPU\s+(\d+)\s+running=(\d+)")

    cpu_ts2nr = defaultdict(dict)
    all_timestamps = set()
    min_running, max_running = float('inf'), float('-inf')
    with open(filename, "r") as f:
        for line in f:
            m1 = pattern_nr_running.search(line)
            m2 = pattern_print_tasks.search(line)
            if m1:
                ts = float(m1.group(1))
                if ts < time_start:
                    continue
                cpu = int(m1.group(2))
                nr_running = int(m1.group(3))
                if cpu in all_cpus:
                    cpu_ts2nr[cpu][ts] = nr_running
                all_timestamps.add(ts)
                min_running = min(min_running, nr_running)
                max_running = max(max_running, nr_running)
            elif m2:
                ts = float(m2.group(1))
                if ts < time_start:
                    continue
                cpu = int(m2.group(2))
                nr_running = int(m2.group(3))
                if cpu in all_cpus:
                    cpu_ts2nr[cpu][ts] = nr_running
                all_timestamps.add(ts)
                min_running = min(min_running, nr_running)
                max_running = max(max_running, nr_running)
    all_timestamps = sorted(all_timestamps)
    cpu_count = len(all_cpus)
    time_count = len(all_timestamps)
    nr_running_matrix = np.full((cpu_count, time_count), np.nan)
    cpu_idx = {cpu: i for i, cpu in enumerate(all_cpus)}
    for cpu in all_cpus:
        for j, ts in enumerate(all_timestamps):
            val = cpu_ts2nr[cpu].get(ts, np.nan)
            nr_running_matrix[cpu_idx[cpu], j] = val
            if not np.isnan(val):
                min_running = min(min_running, val)
                max_running = max(max_running, val)
    return nr_running_matrix, cpu_count, time_count, all_timestamps, int(min_running), int(max_running)

def plot_color_matrix(ax, matrix, cpu_count, time_count, vmin, vmax, timestamps, title_str):
    # x ticks: in ms, offsets from the time_start
    time_start = timestamps[0]
    timestamps_ms = [(t-time_start)*1000 for t in timestamps]
    # Use BoundaryNorm to make discrete color bins
    bounds = np.linspace(vmin - 0.5, vmax + 0.5, N_COLORS + 1)
    norm = colors.BoundaryNorm(boundaries=bounds, ncolors=cmap.N)
    # Make the plot a little looser by adding some padding around the data
    x_min = timestamps_ms[0]
    x_max = timestamps_ms[-1]
    y_min = all_cpus[0] - 0.7  # slightly more space above and below
    y_max = all_cpus[-1] + 0.7

    im = ax.imshow(
        matrix, aspect="auto", interpolation="nearest", cmap=cmap, norm=norm,
        extent=[x_min, x_max, y_min, y_max], origin="lower"
    )
    ax.set_yticks(range(len(all_cpus)))
    ax.set_yticklabels(all_cpus)
    ax.set_ylabel("CPU")
    ax.set_xlabel("Time offset (ms)")
    ax.set_title(title_str)
    # x-ticks
    n_xticks = 6 if len(timestamps_ms) > 6 else len(timestamps_ms)
    x_tick_indices = np.linspace(0, len(timestamps_ms)-1, n_xticks, dtype=int)
    x_ticks = [timestamps_ms[i] for i in x_tick_indices]
    ax.set_xticks(x_ticks)
    ax.set_xticklabels([f"{int(x)}" for x in x_ticks])

    # Create colorbar with discrete color steps only
    cbar = plt.colorbar(im, ax=ax, orientation='vertical', pad=0.03, fraction=0.07, boundaries=bounds, ticks=np.arange(vmin, vmax+1))
    cbar.set_label("nr_running")
    cbar.ax.set_yticklabels([str(int(t)) for t in np.arange(vmin, vmax+1)])

    # Add some whitespace/padding around axes and content
    ax.margins(0.03)
    ax.set_xlim(x_min, x_max)
    ax.set_ylim(y_min, y_max)

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--controller", type=str, default="evenIdleCpu")
    parser.add_argument("--time-start", type=float, default=10.20, help="Only plot data from this time onward (in seconds)")
    args = parser.parse_args()

    bugId = args.controller
    log_file_buggy = f"{LOGS_DIR}/{bugId}_buggy.log"
    title_buggy = f"{bugId} (normal topology)"
    log_file_fixed = f"{LOGS_DIR}/{bugId}_fixed.log"
    title_fixed = f"{bugId} (special topology)"
    output_file = f"{RESULTS_DIR}/{bugId}.pdf"

    # Load data starting from 10.15 seconds (or what user set)
    matrix_buggy, cpu_count, time_count, t_buggy, min_buggy, max_buggy = build_nr_running_matrix(log_file_buggy, time_start=args.time_start)
    matrix_fixed, _, _, t_fixed, min_fixed, max_fixed = build_nr_running_matrix(log_file_fixed, time_start=args.time_start)
    vmin = min(min_buggy, min_fixed)
    vmax = max(max_buggy, max_fixed)

    # Use sharex=False to avoid issues, and give a little more room on y axis
    fig, axes = plt.subplots(2, 1, figsize=(10, 3.8), sharex=False, dpi=200)

    plot_color_matrix(axes[0], matrix_buggy, cpu_count, time_count, vmin, vmax, t_buggy, title_buggy)
    plot_color_matrix(axes[1], matrix_fixed, cpu_count, time_count, vmin, vmax, t_fixed, title_fixed)

    plt.tight_layout()
    # Avoid saving with the 'tight' option for less clipping, and allow more padding
    plt.savefig(output_file, bbox_inches=None, pad_inches=0.2)
