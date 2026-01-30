#!/usr/bin/env python3
"""
Plot util_avg for CPU 2 over time from log files
"""

import argparse
from pathlib import Path

import matplotlib.pyplot as plt
from consts import RESULTS_DIR
from parse import parse_log
from plot_utils import save_fig


def parse_log_file(log_file: Path):
    return parse_log(log_file, prefix="rq")


def plot_util(buggy_df, fixed_df, field: str, ylabel: str):
    fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(1.8, 1.8))
    xmax = max(max(buggy_df["timestamp"]), max(fixed_df["timestamp"]))

    for ax, df, title, color in [
        (ax1, buggy_df, "Buggy", "#A72703"),
        (ax2, fixed_df, "Fixed", "#BBC863"),
    ]:
        ax.plot(
            df["timestamp"],
            df[field],
            linewidth=3,
            linestyle="-",
            color=color,
        )
        ax.set_ylabel(ylabel)
        ax.set_title(title, fontsize=10, pad=3)
        ax.grid(True, alpha=0.3)
        ax.set_yticks([0, 1000])
        ax.set_yticklabels(["0", "1k"])
        ax.set_xlim(0, xmax)

    ax1.set_xticklabels([])
    ax2.set_xlabel("Time (ms)")

    return fig


def main(driver: str):
    buggy_df = parse_log_file(RESULTS_DIR / f"{driver}_buggy.log")
    fixed_df = parse_log_file(RESULTS_DIR / f"{driver}_fixed.log")

    if driver == "uclamp_inversion":
        field = "effective_util"
        ylabel = "Eff. Util"
    else:
        field = "avg_util"
        ylabel = "Avg Util"
    fig = plot_util(buggy_df, fixed_df, field, ylabel)
    save_fig(fig, driver)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("driver", type=str, default="util_avg", nargs="?")
    args = parser.parse_args()
    main(args.driver)
