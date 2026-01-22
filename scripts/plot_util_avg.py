#!/usr/bin/env python3
"""
Plot util_avg for CPU 2 over time from log files
"""

import argparse

import matplotlib.pyplot as plt
from parse import parse_log
from plot_utils import save_fig


def parse_log_file(log_file):
    return parse_log(log_file, prefix="rq")


def plot_util_avg(buggy_df, fixed_df):
    fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(1.8, 1.8))
    xmax = max(max(buggy_df["timestamp"]), max(fixed_df["timestamp"]))

    # Plot buggy version in first subplot
    ax1.plot(
        buggy_df["timestamp"],
        buggy_df["avg_util"],
        linewidth=3,
        linestyle="-",
        color="#A72703",
    )
    ax1.set_ylabel("Avg Util")
    ax1.set_title("Buggy", fontsize=10, pad=3)
    ax1.grid(True, alpha=0.3)
    ax1.set_yticks([0, 1000])
    ax1.set_yticklabels(["0", "1k"])
    ax1.set_xticklabels([])
    ax1.set_xlim(0, xmax)

    # Plot fixed version in second subplot
    ax2.plot(
        fixed_df["timestamp"],
        fixed_df["avg_util"],
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
    ax2.set_xlim(0, xmax)

    plt.tight_layout(pad=0.1)
    return fig


def main(driver: str):
    buggy_df = parse_log_file(f"{driver}_buggy.log")
    fixed_df = parse_log_file(f"{driver}_fixed.log")

    fig = plot_util_avg(buggy_df, fixed_df)
    save_fig(fig, driver)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("driver", type=str, default="util_avg", nargs="?")
    args = parser.parse_args()
    main(args.driver)
