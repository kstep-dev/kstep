#!/usr/bin/env python3

import argparse

import matplotlib.patches as mpatches
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker
import numpy as np
import pandas as pd
from matplotlib import colors
from parse import parse_log
from plot_utils import save_fig


def parse_curr_task(path, type: str) -> pd.DataFrame:
    df = parse_log(path, prefix="task")
    df = df[df["on_cpu"] == True]
    df["type"] = type
    return df


def build_matrix(df: pd.DataFrame, type: str, color_map: dict[int, str]) -> np.ndarray:
    timestamps = df["timestamp"].unique()
    cpus = df["cpu"].unique()

    # Map pids to consecutive integers
    df["pid"], _ = pd.factorize(df["pid"], sort=True)
    df["cpu"] -= min(cpus)

    matrix = np.full((len(cpus), len(timestamps)), -1)
    for _, row in df[df["type"] == type].iterrows():
        matrix[row["cpu"], row["timestamp"]] = row["pid"]

    color_matrix = np.zeros((len(cpus), len(timestamps), 4))
    for i in range(len(cpus)):
        for j in range(len(timestamps)):
            color_matrix[i, j, :] = colors.to_rgba(color_map[matrix[i, j]])
    return color_matrix


def plot_color_matrix(
    df: pd.DataFrame, color_matrix: np.ndarray, ax: plt.Axes, title: str
):
    ax.imshow(color_matrix, aspect="auto", interpolation="nearest")

    # Adjust ticks
    cpu_count = df["cpu"].nunique()
    ax.set_yticks(np.arange(cpu_count))
    ax.set_yticklabels([f"CPU {i + 1}" for i in range(cpu_count)])

    if "Buggy" in title:
        ax.set_xticks([])
        ax.set_xticklabels([])
        ax.set_xlabel("")
    else:
        ax.xaxis.set_major_locator(ticker.MaxNLocator(5, steps=[1, 5, 10]))
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
    df_buggy = parse_curr_task(log_file_buggy, "buggy")
    df_fixed = parse_curr_task(log_file_fixed, "fixed")

    df = pd.concat([df_buggy, df_fixed])
    matrix_buggy = build_matrix(df, "buggy", color_map)
    matrix_fixed = build_matrix(df, "fixed", color_map)

    cpus = df["cpu"].unique()
    if len(cpus) == 1:
        figsize = (4, 0.875)
    else:
        figsize = (4, 1.125)

    fig, (ax_buggy, ax_fixed) = plt.subplots(
        2, 1, figsize=figsize, gridspec_kw={"hspace": 0.75}
    )

    plot_color_matrix(df, matrix_buggy, ax_buggy, title_buggy)
    plot_color_matrix(df, matrix_fixed, ax_fixed, title_fixed)

    handles = []
    labels = []
    unique_pids = sorted(df["pid"].unique())
    for pid in unique_pids:
        handles.append(mpatches.Patch(color=color_map[pid]))
        labels.append(name_map[pid])

    fig.legend(
        handles,
        labels,
        loc="upper center",
        ncol=8,
        bbox_to_anchor=(0.5, 1.25 if len(cpus) == 2 else 1.3),
        fontsize=8,
        handlelength=1,
        handletextpad=0.25,
        columnspacing=1,
        frameon=False,
    )

    return fig


COLOR_IDLE = (0.95, 0.95, 0.95)
COLOR_YELLOW = "#FFE797"
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
        -1: COLOR_IDLE,
        0: COLOR_BUGGY,
        1: COLOR_YELLOW,
        2: COLOR_LIGHT_BLUE,
    },
    "vruntime_overflow": {
        -1: COLOR_IDLE,
        0: COLOR_YELLOW,
        1: COLOR_BUGGY,
        2: COLOR_LIGHT_BLUE,
    },
    "rt_runtime_toggle": {
        0: COLOR_LIGHT_BLUE,
        -1: COLOR_IDLE,
    },
}

NAME_MAPS = {
    "sync_wakeup": {
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
    "rt_runtime_toggle": {
        0: "Real-time Task",
    },
}


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
