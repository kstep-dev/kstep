#!/usr/bin/env python3

import argparse
import re
from collections import defaultdict

import matplotlib.pyplot as plt
import numpy as np
from consts import RESULTS_DIR
from matplotlib import colors

all_cpus = [4, 5, 6, 7]
N_COLORS = 7
cmap = plt.cm.get_cmap('Blues', N_COLORS)

def build_nr_running_matrix(filename, time_start=0.0):
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

def parse_lb_events(filename, time_start=0.0):
    """
    Return a set of (timestamp, cpu) for each LB event with weight == 4 and timestamp >= time_start.
    """
    pattern = re.compile(r'\[\s*([\d\.]+)\]\s+LB\s+(\d+)\s+(\d+)')
    events = set()
    with open(filename, 'r') as f:
        for line in f:
            m = pattern.search(line)
            if m:
                ts = float(m.group(1))
                cpu = int(m.group(2))
                weight = int(m.group(3))
                if weight == 4 and cpu in all_cpus and ts > time_start:
                    events.add((ts, cpu))
    return events

def plot_color_matrix_with_lb(ax, matrix, cpu_count, time_count, vmin, vmax, timestamps, title_str, lb_events):
    # x ticks: in ms, offsets from the time_start
    time_start = timestamps[0]
    timestamps_ms = [(t-time_start)*1000 for t in timestamps]
    bounds = np.linspace(vmin - 0.5, vmax + 0.5, N_COLORS + 1)
    norm = colors.BoundaryNorm(boundaries=bounds, ncolors=cmap.N)
    x_min = timestamps_ms[0]
    x_max = timestamps_ms[-1]
    y_min = all_cpus[0] - 0.7
    y_max = all_cpus[-1] + 0.7

    im = ax.imshow(
        matrix, aspect="auto", interpolation="nearest", cmap=cmap, norm=norm,
        extent=[x_min, x_max, y_min, y_max], origin="lower"
    )
    # Set y-ticks at each integer step, for clear tick marks for CPUs
    yticks = np.arange(all_cpus[0], all_cpus[-1] + 1)
    ax.set_yticks(yticks)
    ax.set_yticklabels([str(cpu) for cpu in yticks])
    ax.set_ylabel("CPU")
    ax.set_xlabel("Time offset (ms)")
    ax.set_title(title_str)
    n_xticks = 6 if len(timestamps_ms) > 6 else len(timestamps_ms)
    x_tick_indices = np.linspace(0, len(timestamps_ms)-1, n_xticks, dtype=int)
    x_ticks = [timestamps_ms[i] for i in x_tick_indices]
    ax.set_xticks(x_ticks)
    ax.set_xticklabels([f"{int(x)}" for x in x_ticks])

    cbar = plt.colorbar(im, ax=ax, orientation='vertical', pad=0.03, fraction=0.07, boundaries=bounds, ticks=np.arange(vmin, vmax+1))
    cbar.set_label("nr_running")
    cbar.ax.set_yticklabels([str(int(t)) for t in np.arange(vmin, vmax+1)])

    ax.margins(0.03)
    ax.set_xlim(x_min, x_max)
    ax.set_ylim(y_min, y_max)

    # ----- Plot dots for lb events -----
    if lb_events:
        # Find mapping: timestamp->index lookup to allow LB dot overlay matching imshow col a time
        ts2idx = {t: i for i, t in enumerate(timestamps)}
        dot_xs = []
        dot_ys = []
        for (lb_ts, lb_cpu) in lb_events:
            # Find closest timestamp in timestamps (within a small tolerance, e.g. 1ms)
            idx = min(range(len(timestamps)), key=lambda i: abs(timestamps[i]-lb_ts)) if timestamps else None
            if idx is not None and abs(timestamps[idx] - lb_ts) < 0.005:
                # Scatter dot at (timestamps_ms[idx], lb_cpu)
                dot_xs.append(timestamps_ms[idx])
                dot_ys.append(lb_cpu)
        ax.scatter(dot_xs, dot_ys, s=30, c='#FF9013', marker='o', label="Try Load Balancing across 4 CPUs", zorder=5)
        if dot_xs:
            ax.legend(loc='upper right', framealpha=0.92)

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--controller", type=str, default="6d7e478")
    parser.add_argument("--time-start", type=float, default=10.20, help="Only plot data from this time onward (in seconds)")
    args = parser.parse_args()

    bugId = args.controller
    log_file_buggy = RESULTS_DIR / f"{bugId}_buggy.log"
    title_buggy = f"{bugId} (buggy)"
    log_file_fixed = RESULTS_DIR / f"{bugId}_fixed.log"
    title_fixed = f"{bugId} (fixed)"
    output_file = RESULTS_DIR / f"{bugId}.pdf"

    matrix_buggy, cpu_count, time_count, t_buggy, min_buggy, max_buggy = build_nr_running_matrix(log_file_buggy, time_start=args.time_start)
    matrix_fixed, _, _, t_fixed, min_fixed, max_fixed = build_nr_running_matrix(log_file_fixed, time_start=args.time_start)
    vmin = min(min_buggy, min_fixed)
    vmax = max(max_buggy, max_fixed)

    # Parse LB events for overlays
    lb_events_buggy = parse_lb_events(log_file_buggy, args.time_start)
    lb_events_fixed = parse_lb_events(log_file_fixed, args.time_start)

    fig, axes = plt.subplots(2, 1, figsize=(6, 3.8), sharex=False, dpi=200)

    plot_color_matrix_with_lb(axes[0], matrix_buggy, cpu_count, time_count, vmin, vmax, t_buggy, title_buggy, lb_events_buggy)
    plot_color_matrix_with_lb(axes[1], matrix_fixed, cpu_count, time_count, vmin, vmax, t_fixed, title_fixed, lb_events_fixed)

    plt.tight_layout()
    plt.savefig(output_file, bbox_inches=None, pad_inches=0.2)
