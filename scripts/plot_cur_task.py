#!/usr/bin/env python3

import argparse

import matplotlib.patches as mpatches
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd
from matplotlib import colors
from parse import parse_log
from plot_utils import save_fig

COLOR_IDLE = (0.95, 0.95, 0.95)
COLOR_YELLOW = "#FFE797"
COLOR_GREEN = "#84994F"
COLOR_ORANGE = "#FCB53B"
COLOR_DARK_BLUE = "#4A70A9"
COLOR_LIGHT_BLUE = "#8FABD4"
COLOR_BUGGY = "tab:red"

COLOR_MAPS = {
    "sync_wakeup": {
        -1: COLOR_IDLE,
        0: COLOR_YELLOW,
        1: COLOR_LIGHT_BLUE,
        2: COLOR_BUGGY,
    },
    "freeze": {
        0: COLOR_BUGGY,
        1: COLOR_YELLOW,
        2: COLOR_LIGHT_BLUE,
    },
    "vruntime_overflow": {
        0: COLOR_YELLOW,
        1: COLOR_BUGGY,
        2: COLOR_LIGHT_BLUE,
    },
}

NAME_MAPS = {
    "sync_wakeup": {
        -1: "Idle",
        0: "Task 1",
        1: "Task 2 (waker)",
        2: "Task 3 (wakee)",
    },
    "freeze": {
        0: "Task 1 (fail to freeze)",
        1: "Task 2",
        2: "Task 3",
    },
    "vruntime_overflow": {
        0: "Task 1",
        1: "Task 2 (starved)",
        2: "Task 3",
    },
}


def parse_curr_task(path) -> pd.DataFrame:
    df = parse_log(path, prefix="task")
    df = df[df["on_cpu"] == True]
    return df


def build_pid_matrix(df: pd.DataFrame) -> pd.DataFrame:
    # Normalize pids to start from 0 and be consecutive
    df["pid"], _ = pd.factorize(df["pid"], sort=True)
    # Transform table to matrix
    return (
        df.pivot(
            index=["name", "timestamp"],
            columns=["cpu"],
            values="pid",
        )
        .replace(np.nan, -1)
        .astype(int)
    )


def plot_color_matrix(
    pid_matrix: pd.DataFrame, ax: plt.Axes, title: str, color_map: dict[int, str]
):
    # Map pids to colors and convert to numpy array
    color_df = pid_matrix.map(lambda x: colors.to_rgba(color_map[x]))
    color_matrix = np.array(color_df.T.values.tolist())

    # Plot color matrix
    ax.imshow(color_matrix, aspect="auto", interpolation="nearest")

    # Adjust ticks
    cpu_count = color_matrix.shape[0]
    ax.set_yticks(np.arange(cpu_count))
    ax.set_yticklabels([f"CPU {i + 1}" for i in range(cpu_count)])

    if "Buggy" in title:
        ax.set_xticks([])
        ax.set_xticklabels([])
        ax.set_xlabel("")
    else:
        time_count = color_matrix.shape[1]
        xtick_locs = np.arange(0, time_count, 10)
        # Ensure the last tick is included if not already
        if len(xtick_locs) == 0 or xtick_locs[-1] != time_count - 1:
            xtick_locs = np.append(xtick_locs, time_count - 1)

        ax.set_xticks(xtick_locs)
        ax.set_xticklabels([f"{i}" for i in xtick_locs])
        ax.tick_params(axis="x", length=2, pad=1)  # Shorter ticks, labels closer
        ax.set_xlabel("Time (ms)", labelpad=0.5)

    ax.set_title(title, fontsize=10, pad=3)


def plot_cur_task(
    log_file_buggy,
    log_file_fixed,
    title_buggy: str,
    title_fixed: str,
    color_map: dict[int, str],
    name_map: dict[int, str],
):
    df_buggy = parse_curr_task(log_file_buggy)
    df_fixed = parse_curr_task(log_file_fixed)
    df_buggy["name"] = "buggy"
    df_fixed["name"] = "fixed"
    df = build_pid_matrix(pd.concat([df_buggy, df_fixed]))

    num_cpus = df.shape[1]

    if num_cpus == 1:
        figsize = (4, 0.875)
    else:
        figsize = (4, 1.125)

    fig, (ax_buggy, ax_fixed) = plt.subplots(
        2, 1, figsize=figsize, gridspec_kw={"hspace": 0.75}
    )

    plot_color_matrix(df.loc["buggy"], ax_buggy, title_buggy, color_map)
    plot_color_matrix(df.loc["fixed"], ax_fixed, title_fixed, color_map)

    handles = []
    labels = []
    unique_pids = np.unique(df.values.flatten())
    for pid in unique_pids:
        handles.append(mpatches.Patch(color=color_map[pid]))
        labels.append(name_map[pid])

    fig.legend(
        handles,
        labels,
        loc="upper center",
        ncol=8,
        bbox_to_anchor=(0.5, 1.25 if num_cpus == 2 else 1.3),
        fontsize=8,
        handlelength=1,
        handletextpad=0.25,
        columnspacing=1,
        frameon=False,
    )

    return fig


def main(driver: str):
    if driver == "sync_wakeup":
        title_buggy = "Buggy and Official Fix"
        title_fixed = "Our Fix"
    else:
        title_buggy = "Buggy"
        title_fixed = "Fixed"

    fig = plot_cur_task(
        log_file_buggy=f"{driver}_buggy.log",
        log_file_fixed=f"{driver}_fixed.log",
        title_buggy=title_buggy,
        title_fixed=title_fixed,
        color_map=COLOR_MAPS.get(driver, {}),
        name_map=NAME_MAPS.get(driver, {}),
    )

    save_fig(fig, driver)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("driver", type=str, default="sync_wakeup", nargs="?")
    args = parser.parse_args()
    main(args.driver)
