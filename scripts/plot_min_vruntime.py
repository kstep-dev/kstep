#!/usr/bin/env python3
"""
Plot min_vruntime and avg_vruntime for CPU 2 over time from log files
"""

import argparse
import re

import matplotlib.pyplot as plt
from consts import RESULTS_DIR

init_timestamp = 10.0

def parse_log_file(log_file):
    """
    Parse the log file and extract timestamp, min_vruntime, and avg_vruntime for CPU 2

    Expected format:
    [timestamp] ... print_tasks: - CPU 2 ... min_vruntime=VALUE ... avg_vruntime=VALUE
    avg_vruntime may be absent in old kernels or log files.
    """
    timestamps = []
    min_vruntime_values = []
    avg_vruntime_values = []

    # Pattern to match lines with CPU 2, min_vruntime, and optionally avg_vruntime
    pattern = r'\[\s*(\d+\.\d+)\].*CPU 2.*min_vruntime=([0-9]+)(?:.*avg_vruntime=([0-9]+))?'

    with open(log_file, 'r') as f:
        for line in f:
            match = re.search(pattern, line)
            if match and float(match.group(1)) >= init_timestamp:
                timestamp = (float(match.group(1)) - init_timestamp) * 1000
                min_vruntime = int(match.group(2))
                if match.group(3) is not None:
                    avg_vruntime = int(match.group(3))
                else:
                    avg_vruntime = None
                timestamps.append(timestamp)
                min_vruntime_values.append(min_vruntime)
                avg_vruntime_values.append(avg_vruntime)
    print(f"min_vruntime samples: {len(timestamps)}")
    return timestamps, min_vruntime_values, avg_vruntime_values

def plot_min_avg_vruntime(
    timestamps_buggy, min_vruntime_buggy, avg_vruntime_buggy,
    timestamps_fixed, min_vruntime_fixed, avg_vruntime_fixed,
    bugId, output_file
):
    """
    Plot min_vruntime and avg_vruntime for buggy and fixed in two subplots.
    Top: min_vruntime as scatter.
    Bottom: avg_vruntime as scatter using same shape, size.
    """
    # Make both subplots the same size as in plot_util_avg: two rows, one column, figsize=(3,2.5)
    fig, ax1 = plt.subplots(1, figsize=(3, 2.5), sharex=True)

    # Top subplot: min_vruntime using scatter
    ax1.scatter(
        timestamps_buggy, min_vruntime_buggy,
        s=36,
        marker='o',
        facecolor='#A72703',
        edgecolor='black',
        linewidth=0.5,
        label='Buggy',
        alpha=0.95,
        zorder=2
    )
    ax1.scatter(
        timestamps_fixed, min_vruntime_fixed,
        s=60,
        marker='s',
        facecolor='none',
        edgecolor='#BBC863',
        linewidth=1.8,
        label='Fixed',
        alpha=0.95,
        zorder=3
    )
    # ax1.set_xlim(left = 5000)
    ax1.set_ylabel('min_vruntime', fontsize = 13)
    ax1.set_title(f'Lag_Vruntime', fontsize=13)
    ax1.grid(True, alpha=0.3)
    if len(timestamps_buggy) > 0 and len(timestamps_fixed) > 0:
        ax1.set_xlim(5, max(max(timestamps_buggy), max(timestamps_fixed)) - 4)
    ax1.legend()
    ax1.tick_params(axis='both', which='major', labelsize=13)

    # Bottom subplot: avg_vruntime as scatter plot if we have those values.
    # Only plot when at least one value is not None and enough points
    def _valid_points(ts, vs):
        return [(t, v) for t, v in zip(ts, vs) if v is not None]

    buggy_valid = _valid_points(timestamps_buggy, avg_vruntime_buggy)
    fixed_valid = _valid_points(timestamps_fixed, avg_vruntime_fixed)

    shown_any = False

    """
    if buggy_valid:
        t_buggy, v_buggy = zip(*buggy_valid)
        ax2.scatter(
            t_buggy, v_buggy,
            s=36,
            marker='o',
            facecolor='#A72703',
            edgecolor='black',
            linewidth=0.5,
            label='Buggy',
            alpha=0.95,
            zorder=2
        )
        shown_any = True
    if fixed_valid:
        t_fixed, v_fixed = zip(*fixed_valid)
        ax2.scatter(
            t_fixed, v_fixed,
            s=60,
            marker='s',
            facecolor='none',
            edgecolor='#BBC863',
            linewidth=1.8,
            label='Fixed',
            alpha=0.95,
            zorder=3
        )
        shown_any = True
    ax2.set_xlabel('Time (ms)')
    ax2.set_ylabel('avg_vruntime')
    ax2.grid(True, alpha=0.3)
    ax2.set_title(f'{bugId} - avg_vruntime')
    # ax2.sharex with ax1 by default via sharex=True
    # Legend only if we drew at least one valid plot
    if shown_any:
        ax2.legend()
    else:
        # If no data, indicate it
        ax2.text(
            0.5, 0.5, "No avg_vruntime data found",
            transform=ax2.transAxes, ha='center', va='center', fontsize=10, color='grey'
        )

    """
    ax1.set_xlabel('Time (ms)', fontsize = 13)
    plt.tight_layout(pad = 0.1)
    plt.savefig(output_file)

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--controller", type=str, default="lag_vruntime") # 5068d84
    args = parser.parse_args()

    bugId = args.controller

    # Paths to the log files
    buggy_log = RESULTS_DIR / f"{bugId}_buggy.log"
    fixed_log = RESULTS_DIR / f"{bugId}_fixed.log"

    output_file = RESULTS_DIR / f"{bugId}.pdf"

    print(f"Parsing log file: {buggy_log}")
    timestamps_buggy, min_vruntime_buggy, avg_vruntime_buggy = parse_log_file(buggy_log)
    timestamps_fixed, min_vruntime_fixed, avg_vruntime_fixed = parse_log_file(fixed_log)

    plot_min_avg_vruntime(
        timestamps_buggy, min_vruntime_buggy, avg_vruntime_buggy,
        timestamps_fixed, min_vruntime_fixed, avg_vruntime_fixed,
        bugId, output_file
    )

if __name__ == '__main__':
    main()
