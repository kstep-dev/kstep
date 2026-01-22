#!/usr/bin/env python3
"""
Script to parse rebalance domain logs and plot overhead over time.
"""

import argparse

import matplotlib.pyplot as plt
from parse import parse_log
from plot_utils import save_fig


def parse_log_file(path, target_cpu, time_start=1000):
    df = parse_log(path, prefix="sched_softirq")
    df = df[df["timestamp"] > time_start]
    df["timestamp"] -= time_start
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
    ax.set_ylim(0, 4)
    ax.set_xlim(left=0)
    ax.legend(bbox_to_anchor=(1.01, 0.5), loc="upper right", borderaxespad=0.2)
    ax.grid(True, alpha=0.3)

    return fig


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--controller", type=str, default="long_balance")
    args = parser.parse_args()

    target_cpu = 2
    buggy_df = parse_log_file(f"{args.controller}_buggy.log", target_cpu=target_cpu)
    fixed_df = parse_log_file(f"{args.controller}_fixed.log", target_cpu=target_cpu)

    fig = plot_rebalance_comparison(buggy_df, fixed_df)

    save_fig(fig, args.controller)


if __name__ == "__main__":
    main()
