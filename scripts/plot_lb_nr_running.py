#!/usr/bin/env python3

import argparse

import matplotlib.lines as mlines
import matplotlib.patches as mpatches
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker
import numpy as np
from consts import RESULTS_DIR
from matplotlib import colors
from parse import parse_load_balance, parse_nr_running
from plot_utils import save_fig

all_cpus = [4, 5, 6, 7]
N_COLORS = 4
cmap = plt.cm.get_cmap("Blues", N_COLORS)


def build_nr_running_matrix(filename):
    df = parse_nr_running(filename)
    df = df[df["cpu"].isin(all_cpus)]
    nr_running_matrix = df.pivot_table(
        index="timestamp", columns="cpu", values="val", aggfunc="first"
    ).T
    all_timestamps = nr_running_matrix.columns

    return (
        nr_running_matrix,
        all_timestamps,
        nr_running_matrix.min().min(),
        nr_running_matrix.max().max(),
    )


def parse_lb_events(filename):
    df = parse_load_balance(filename)
    df = df[df["timestamp"] != 0]
    return df


def plot_color_matrix_with_lb(ax, matrix, vmin, vmax, timestamps, title_str, lb_events):
    # x ticks: in ms, offsets from the time_start
    bounds = np.linspace(vmin - 0.5, vmax + 0.5, N_COLORS + 1)
    norm = colors.BoundaryNorm(boundaries=bounds, ncolors=cmap.N)
    x_min = timestamps[0]
    x_max = timestamps[-1]
    y_min = all_cpus[0] - 0.7
    y_max = all_cpus[-1] + 0.7

    ax.imshow(
        matrix,
        aspect="auto",
        interpolation="nearest",
        cmap=cmap,
        norm=norm,
        extent=[x_min, x_max, y_min, y_max],
        origin="lower",
    )
    # Set y-ticks at each integer step, for clear tick marks for CPUs
    yticks = np.arange(all_cpus[0], all_cpus[-1] + 1)
    ax.set_yticks(yticks)
    ax.set_yticklabels([i + 1 for i in range(len(yticks))])
    ax.set_ylabel("CPU", fontsize=13)

    ax.set_title(title_str, fontsize=12, pad=3)

    if "Buggy" in title_str:
        ax.set_xticks([])
        ax.set_xlabel("")
    else:
        ax.xaxis.set_major_locator(ticker.MaxNLocator(3))
        # Shorter ticks, labels closer
        ax.tick_params(axis="x", length=2, pad=1, labelsize=13)
        ax.set_xlabel("Time (ms)", labelpad=0.5, fontsize=13)

    ax.margins(0.03)
    ax.set_xlim(x_min, x_max + 1)
    ax.set_ylim(y_min, y_max)

    # ----- Plot dots for lb events -----
    if bugId == "even_idle_cpu":
        dot_size = 10
    else:
        dot_size = 30
    ax.scatter(
        lb_events["timestamp"],
        lb_events["dst_cpu"],
        s=dot_size,
        c="#FF9013",
        marker="o",
        zorder=5,
    )


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--controller", type=str, default="extra_balance")  # 6d7e478
    args = parser.parse_args()

    bugId = args.controller
    log_file_buggy = RESULTS_DIR / f"{bugId}_buggy.log"
    title_buggy = "Buggy"
    log_file_fixed = RESULTS_DIR / f"{bugId}_fixed.log"
    title_fixed = "Fixed"
    output_file = RESULTS_DIR / f"{bugId}.pdf"

    if bugId == "even_idle_cpu":
        all_cpus = [1, 2, 3, 4]
    elif bugId == "extra_balance":
        all_cpus = [4, 5, 6, 7]

    matrix_buggy, t_buggy, min_buggy, max_buggy = build_nr_running_matrix(
        log_file_buggy
    )
    matrix_fixed, t_fixed, min_fixed, max_fixed = build_nr_running_matrix(
        log_file_fixed
    )
    vmin = min(min_buggy, min_fixed)
    vmax = max(max_buggy, max_fixed)

    # Parse LB events for overlays
    lb_events_buggy = parse_lb_events(log_file_buggy)
    lb_events_fixed = parse_lb_events(log_file_fixed)
    if bugId == "even_idle_cpu":
        lb_events_buggy = lb_events_buggy[lb_events_buggy["name"] == "MC"]
        lb_events_fixed = lb_events_fixed[lb_events_fixed["name"] == "MC"]
    elif bugId == "extra_balance":
        lb_events_buggy = lb_events_buggy[lb_events_buggy["span"] == "4-7"]
        lb_events_fixed = lb_events_fixed[lb_events_fixed["span"] == "4-7"]

    fig, axes = plt.subplots(
        2, 1, figsize=(3.5, 2), sharex=False, gridspec_kw={"hspace": 0.3}
    )

    plot_color_matrix_with_lb(
        axes[0],
        matrix_buggy,
        vmin,
        vmax,
        t_buggy,
        title_buggy,
        lb_events_buggy,
    )
    plot_color_matrix_with_lb(
        axes[1],
        matrix_fixed,
        vmin,
        vmax,
        t_fixed,
        title_fixed,
        lb_events_fixed,
    )

    handles = []
    labels = []
    handles.append(mlines.Line2D([], [], color="#FF9013", marker="o", linestyle="None"))
    if bugId == "even_idle_cpu":
        labels.append("Balance \nacross\n2-CPU group\n")
    else:
        labels.append("Balance \nin 4-CPU\ndomain")
    for i in range(N_COLORS):
        handles.append(mpatches.Patch(color=cmap(i)))
        labels.append(f"{i} running")
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

    save_fig(fig, output_file)
