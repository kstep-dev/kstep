#!/usr/bin/env python3
"""
Plot util_avg for CPU 2 over time from log files
"""

import argparse

import matplotlib.pyplot as plt
import matplotlib.ticker as ticker
from utils import ResultDir
from parse_log import parse_jsonl
from plot_utils import save_fig


def plot_util(buggy_df, fixed_df, ylabel: str):
    fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(1.8, 1.8), sharex=True, sharey=True)
    ymax = max(max(buggy_df["val"]), max(fixed_df["val"]))

    for ax, df, title, color in [
        (ax1, buggy_df, "Buggy", "#A72703"),
        (ax2, fixed_df, "Fixed", "#BBC863"),
    ]:
        ax.plot(
            df["timestamp"],
            df["val"],
            linewidth=3,
            linestyle="-",
            color=color,
        )
        ax.set_title(title, fontsize=10, pad=3)
        ax.grid(True, alpha=0.3)

        if ymax > 500:
            ax.set_ylim(0, 1100)
        else:
            ax.set_ylim(0, int(round(ymax / 100.0)) * 100)
        ax.margins(x=0)

        ax.xaxis.set_major_locator(
            ticker.MaxNLocator(3, steps=[1, 5, 10], integer=True)
        )
        ax.yaxis.set_major_locator(ticker.MaxNLocator(1, integer=True))
        ax.yaxis.set_major_formatter(
            ticker.FuncFormatter(
                lambda x, pos: f"{x:.0f}" if x < 1000 else f"{int(x / 1000)}k"
            )
        )

    ax2.set_xlabel("Time (ms)")
    fig.supylabel(ylabel, x=0)

    return fig


def main(driver: str):
    if driver == "uclamp_inversion":
        type = "effective_util"
        ylabel = "Effective Utilization"
    elif driver == "h_nr_runnable":
        type = "runnable_avg"
        ylabel = "Runnable Avg"
    elif driver == "util_avg":
        type = "avg_util"
        ylabel = "Average Utilization"
    else:
        type = "avg_util"
        ylabel = "Average Utilization"
    buggy_df = parse_jsonl(ResultDir(f"repro_{driver}/buggy").output, type)
    fixed_df = parse_jsonl(ResultDir(f"repro_{driver}/fixed").output, type)

    fig = plot_util(buggy_df, fixed_df, ylabel)
    save_fig(fig, driver)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("driver", type=str, default="util_avg", nargs="?")
    args = parser.parse_args()
    main(args.driver)
