#!/usr/bin/env python3
"""
Plot util_avg for CPU 2 over time from log files
"""

import argparse

import matplotlib.pyplot as plt
from consts import RESULTS_DIR
from parse import parse_log
from plot_utils import save_fig


def parse_log_file(log_file):
    df = parse_log(log_file, prefix="rq")
    return df["timestamp"], df["avg_util"]


def plot_util_avg(
    timestamps_buggy,
    util_avg_values_buggy,
    timestamps_fixed,
    util_avg_values_fixed,
):
    """
    Plot util_avg over time in two subplots
    """
    fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(1.8, 1.8))

    # Plot buggy version in first subplot
    ax1.plot(
        timestamps_buggy,
        util_avg_values_buggy,
        linewidth=3,
        linestyle="-",
        color="#A72703",
    )
    # ax1.set_xlabel('Time (ms)')
    ax1.set_ylabel("Avg Util")
    ax1.set_title("Buggy", fontsize=10, pad=3)
    ax1.grid(True, alpha=0.3)
    ax1.set_yticks([0, 1000])
    ax1.set_yticklabels(["0", "1k"])
    ax1.set_xticklabels([])
    ax1.set_xlim(0, max(max(timestamps_buggy), max(timestamps_fixed)))

    # Plot fixed version in second subplot
    ax2.plot(
        timestamps_fixed,
        util_avg_values_fixed,
        linewidth=3,
        linestyle="-",
        color="#BBC863",
    )
    ax2.set_xlabel("Time (ms)")
    ax2.set_ylabel("Avg Util")
    ax2.set_yticks([0, 1000])
    ax2.set_yticklabels(["0", "1k"])
    ax2.set_title("Fixed", fontsize=10, pad=3)
    ax2.grid(True, alpha=0.3)
    ax2.set_xlim(0, max(max(timestamps_buggy), max(timestamps_fixed)))

    plt.tight_layout(pad=0.1)
    return fig


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--controller", type=str, default="util_avg")
    args = parser.parse_args()

    # Paths to the log files
    buggy_log = RESULTS_DIR / f"{args.controller}_buggy.log"
    fixed_log = RESULTS_DIR / f"{args.controller}_fixed.log"

    timestamps_buggy, util_avg_values_buggy = parse_log_file(buggy_log)
    timestamps_fixed, util_avg_values_fixed = parse_log_file(fixed_log)

    fig = plot_util_avg(
        timestamps_buggy, util_avg_values_buggy, timestamps_fixed, util_avg_values_fixed
    )
    save_fig(fig, args.controller)


if __name__ == "__main__":
    main()
