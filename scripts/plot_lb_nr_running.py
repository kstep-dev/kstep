#!/usr/bin/env python3

import argparse
from pathlib import Path

import matplotlib.lines as mlines
import matplotlib.patches as mpatches
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker
import numpy as np
import pandas as pd
from consts import RESULTS_DIR
from parse_log import parse_jsonl
from plot_utils import save_fig


def parse_nr_running(path: Path) -> pd.DataFrame:
    return parse_jsonl(path, "nr_running").set_index("timestamp", drop=True)


def parse_lb_events(path: Path, cpus: list[int], driver: str) -> pd.DataFrame:
    df = parse_jsonl(path, "load_balance")
    if df.empty:
        return pd.DataFrame(columns=["timestamp", "dst_cpu", "name"])
    required_cols = {"timestamp", "dst_cpu", "name"}
    if not required_cols.issubset(df.columns):
        return pd.DataFrame(columns=["timestamp", "dst_cpu", "name"])
    df = df[df["timestamp"] != 0]
    df["dst_cpu"] -= min(cpus)
    if driver in {"extra_balance", "local_group_imbalance"}:
        df = df[df["name"] == "MC"]
    return df


def build_cmap(nr_running_df):
    vmin = nr_running_df.min().min()
    vmax = nr_running_df.max().max()
    nr_colors = vmax - vmin + 1
    cmap = plt.cm.get_cmap("Blues", nr_colors)
    return {i: cmap(i) for i in range(vmin, vmax + 1)}


def plot_color_matrix_with_lb(
    ax: plt.Axes,
    nr_running_df: pd.DataFrame,
    lb_events: pd.DataFrame,
    driver: str,
    cmap,
    title: str,
):
    values = nr_running_df.to_numpy(dtype=int).T
    matrix = np.empty((values.shape[0], values.shape[1], 4), dtype=float)
    for k, rgba in cmap.items():
        matrix[values == k] = rgba
    ax.imshow(matrix, aspect="auto", origin="lower", interpolation="nearest")

    ax.set_xlim(nr_running_df.index.min(), nr_running_df.index.max() + 1)
    ax.yaxis.set_major_locator(ticker.MaxNLocator(integer=True))
    ax.set_ylabel("CPU", fontsize=13)

    ax.set_title(title, fontsize=12, pad=3)

    if "Buggy" in title:
        ax.set_xticks([])
        ax.set_xlabel("")
    else:
        ax.xaxis.set_major_locator(ticker.MaxNLocator(4, steps=[1, 5, 10]))
        # Shorter ticks, labels closer
        ax.tick_params(axis="x", length=2, pad=1, labelsize=13)
        ax.set_xlabel("Time (ms)", labelpad=0.5, fontsize=13)

    # ----- Plot dots for lb events -----
    if lb_events.empty:
        return
    ax.scatter(
        lb_events["timestamp"],
        lb_events["dst_cpu"],
        s=10 if driver in {"even_idle_cpu", "local_group_imbalance"} else 30,
        c="#FF9013",
        marker="o",
        zorder=5,
    )


def plot_legend(fig, driver, cmap):
    handles = []
    labels = []
    handles.append(mlines.Line2D([], [], color="#FF9013", marker="o", linestyle="None"))
    if driver in {"even_idle_cpu", "local_group_imbalance"}:
        labels.append("Balance \nacross\n2-CPU group\n")
    else:
        labels.append("Balance \nin 4-CPU\ndomain\n")
    for k, v in cmap.items():
        handles.append(mpatches.Patch(color=v))
        labels.append(f"{k} running")
    fig.legend(
        handles,
        labels,
        loc="center right",
        bbox_to_anchor=(1.25, 0.5),
        borderaxespad=0.0,
        handlelength=1,
        handletextpad=0.5,
        frameon=False,
    )


def main(driver: str):
    out_file_buggy = RESULTS_DIR / f"{driver}_buggy.jsonl"
    out_file_fixed = RESULTS_DIR / f"{driver}_fixed.jsonl"

    nr_running_buggy = parse_nr_running(out_file_buggy)
    nr_running_fixed = parse_nr_running(out_file_fixed)

    cpus = [1, 2, 3, 4]
    lb_events_buggy = parse_lb_events(out_file_buggy, cpus=cpus, driver=driver)
    lb_events_fixed = parse_lb_events(out_file_fixed, cpus=cpus, driver=driver)

    fig, (ax1, ax2) = plt.subplots(
        2, 1, figsize=(3.5, 2), sharex=False, gridspec_kw={"hspace": 0.3}
    )

    cmap = build_cmap(pd.concat([nr_running_buggy, nr_running_fixed]))
    plot_color_matrix_with_lb(
        ax1, nr_running_buggy, lb_events_buggy, driver, cmap, title="Buggy"
    )
    plot_color_matrix_with_lb(
        ax2, nr_running_fixed, lb_events_fixed, driver, cmap, title="Fixed"
    )
    plot_legend(fig, driver, cmap)
    save_fig(fig, driver)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("driver", type=str, default="extra_balance", nargs="?")
    args = parser.parse_args()
    main(args.driver)
