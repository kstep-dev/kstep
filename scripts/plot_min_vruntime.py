#!/usr/bin/env python3
"""
Plot min_vruntime and avg_vruntime for CPU 2 over time from log files
"""

import argparse
from pathlib import Path

import matplotlib.pyplot as plt
from consts import RESULTS_DIR
from parse import parse_log
from plot_utils import save_fig


def parse_log_file(path: Path):
    df = parse_log(path, prefix="rq")
    df = df[df["cpu"] == 1]
    return df


def plot_min_vruntime(buggy_df, fixed_df):
    fig, ax1 = plt.subplots(figsize=(1.95, 1.95))

    # Top subplot: min_vruntime using scatter
    ax1.scatter(
        buggy_df["timestamp"],
        buggy_df["min_vruntime"],
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
        fixed_df["timestamp"],
        fixed_df["min_vruntime"],
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
    ax1.set_ylabel("min_vruntime", fontsize=13)
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
    return fig


def main(driver: str):
    buggy_df = parse_log_file(RESULTS_DIR / f"{driver}_buggy.log")
    fixed_df = parse_log_file(RESULTS_DIR / f"{driver}_fixed.log")

    fig = plot_min_vruntime(buggy_df, fixed_df)
    save_fig(fig, driver)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("driver", type=str, default="lag_vruntime", nargs="?")
    args = parser.parse_args()
    main(args.driver)
