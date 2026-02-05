#!/usr/bin/env python3
"""
Plot util_avg for CPU 2 over time from log files
"""

import argparse

import matplotlib.pyplot as plt
import matplotlib.ticker as ticker
from consts import RESULTS_DIR
from parse_log import parse_log
from plot_utils import save_fig


def plot_util(buggy_df, fixed_df, field: str, ylabel: str):
    fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(1.8, 1.8), sharex=True, sharey=True)
    ymax = max(max(buggy_df[field]), max(fixed_df[field]))

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
        field = "effective_util"
        ylabel = "Effective Utilization"
        prefix = "on_tick"
    elif driver == "h_nr_runnable":
        field = "runnable_avg"
        ylabel = "Runnable Avg"
        prefix = "rq"
    else:
        field = "avg_util"
        ylabel = "Average Utilization"
        prefix = "rq"

    buggy_df = parse_log(RESULTS_DIR / f"{driver}_buggy.log", prefix=prefix)
    fixed_df = parse_log(RESULTS_DIR / f"{driver}_fixed.log", prefix=prefix)

    fig = plot_util(buggy_df, fixed_df, field, ylabel)
    save_fig(fig, driver)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("driver", type=str, default="util_avg", nargs="?")
    args = parser.parse_args()
    main(args.driver)
