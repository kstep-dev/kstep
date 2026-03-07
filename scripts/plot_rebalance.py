#!/usr/bin/env python3
"""
Script to parse rebalance domain logs and plot overhead over time.
"""

import argparse
from pathlib import Path

import matplotlib.pyplot as plt
import matplotlib.ticker as ticker
from consts import RESULTS_DIR
from parse_log import parse_jsonl
from plot_utils import save_fig


def parse_log_file(path: Path, target_cpu: int):    
    df = parse_jsonl(path, type="sched_softirq")
    df["lat_ms"] = df["lat_us"] / 1000.0
    df = df[df["cpu"] == target_cpu]
    return df


def plot_rebalance_comparison(buggy_df, fixed_df):
    fig, ax = plt.subplots(figsize=(1.8, 1.8))

    # Plot buggy version data for target CPU
    ax.scatter(
        buggy_df["timestamp"], buggy_df["lat_ms"], label="Buggy", color="#A72703"
    )
    ax.scatter(
        fixed_df["timestamp"], fixed_df["lat_ms"], label="Fixed", color="#BBC863"
    )

    ax.set_xlabel("Time (ms)")
    ax.set_ylabel("Balance Overhead (ms)")
    ax.yaxis.label.set_position((0, 0.4))
    ax.set_ylim(0, 12)
    ax.set_xlim(left=0)
    ax.yaxis.set_major_locator(ticker.MaxNLocator(4, integer=True))
    ax.legend(bbox_to_anchor=(1.01, 0.5), loc="upper right", borderaxespad=0.2)
    ax.grid(True, alpha=0.3)

    return fig


def main(driver: str):
    target_cpu = 2
    buggy_df = parse_log_file(RESULTS_DIR / f"{driver}_buggy.jsonl", target_cpu)
    fixed_df = parse_log_file(RESULTS_DIR / f"{driver}_fixed.jsonl", target_cpu)

    fig = plot_rebalance_comparison(buggy_df, fixed_df)
    save_fig(fig, driver)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("driver", type=str, default="long_balance", nargs="?")
    args = parser.parse_args()
    main(args.driver)
