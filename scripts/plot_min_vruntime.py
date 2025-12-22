#!/usr/bin/env python3
"""
Plot min_vruntime and avg_vruntime for CPU 2 over time from log files
"""

import argparse

import matplotlib.pyplot as plt
from consts import RESULTS_DIR
from parse import parse_rq
from plot_utils import save_fig


def parse_log_file(log_file):
    df = parse_rq(log_file)
    df = df[df["cpu"] == 1]
    return df["timestamp"], df["min_vruntime"], df["avg_vruntime"]


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
    fig, ax1 = plt.subplots(1, figsize=(2.25, 2.25))

    # Top subplot: min_vruntime using scatter
    ax1.scatter(
        timestamps_buggy,
        min_vruntime_buggy,
        s=26,
        marker="o",
        facecolor="#A72703",
        edgecolor="black",
        linewidth=0.5,
        label="Buggy",
        alpha=0.95,
        zorder=2,
    )
    ax1.scatter(
        timestamps_fixed,
        min_vruntime_fixed,
        s=60,
        marker="s",
        facecolor="none",
        edgecolor="#BBC863",
        linewidth=1.8,
        label="Fixed",
        alpha=0.95,
        zorder=3,
    )
    # ax1.set_xlim(left = 5000)
    ax1.set_ylabel('min_vruntime', fontsize = 13)
    # ax1.set_title(f'Lag_Vruntime', fontsize=13)
    ax1.grid(True, alpha=0.3)
    # show end x-axis label
    # ax1.set_xlim(0, max(max(timestamps_buggy), max(timestamps_fixed)) + 1)
    ax1.set_xticks([0, 5, 10])
    ax1.legend(
        handlelength=0.5,
        handletextpad=0.5,
    )
    ax1.tick_params(axis="both", which="major", labelsize=13)
    ax1.set_xlabel("Time (ms)", fontsize=13)
    save_fig(fig, output_file)

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
