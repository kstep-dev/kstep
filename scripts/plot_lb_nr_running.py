#!/usr/bin/env python3

import argparse

import matplotlib.lines as mlines
import matplotlib.patches as mpatches
import matplotlib.pyplot as plt
import numpy as np
from consts import RESULTS_DIR
from matplotlib import colors
from parse import parse_load_balance, parse_nr_running
from plot_utils import save_fig

all_cpus = [1, 2, 3, 4]
N_COLORS = 4
cmap = plt.cm.get_cmap("Blues", N_COLORS)


def build_nr_running_matrix(filename, time_start=0.0):
    df = parse_nr_running(filename)
    nr_running_matrix = df.pivot_table(
        index="timestamp", columns="cpu", values="val", aggfunc="first"
    ).T
    cpu_count = nr_running_matrix.shape[0]
    time_count = nr_running_matrix.shape[1]
    all_timestamps = nr_running_matrix.columns
    min_running = nr_running_matrix.min().min()
    max_running = nr_running_matrix.max().max()

    return (
        nr_running_matrix,
        cpu_count,
        time_count,
        all_timestamps,
        int(min_running),
        int(max_running),
    )


def parse_lb_events(filename, time_start=0.0):
    df = parse_load_balance(filename)
    df = df[df["name"] == "MC"]
    df = df[df["timestamp"] != 0]
    return df


def plot_color_matrix_with_lb(
    ax, matrix, cpu_count, time_count, vmin, vmax, timestamps, title_str, lb_events
):
    # x ticks: in ms, offsets from the time_start
    bounds = np.linspace(vmin - 0.5, vmax + 0.5, N_COLORS + 1)
    norm = colors.BoundaryNorm(boundaries=bounds, ncolors=cmap.N)
    x_min = timestamps[0]
    x_max = timestamps[-1]
    y_min = all_cpus[0] - 0.7
    y_max = all_cpus[-1] + 0.7

    im = ax.imshow(
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
    ax.set_yticklabels([str(cpu) for cpu in yticks])
    ax.set_ylabel("CPU", fontsize=13)

    ax.set_title(title_str, fontsize=12, pad=3)

    if "Buggy" in title_str:
        ax.set_xticks([])
        ax.set_xlabel("")
    else:
        tick_start = int(np.ceil(timestamps[0] / 100) * 100)
        tick_end = int(np.floor(timestamps[-1] / 100) * 100)
        x_ticks = list(range(tick_start, tick_end + 1, 100))
        ax.set_xticks(x_ticks)
        ax.set_xticklabels([f"{int(x)}" for x in x_ticks], fontsize=13)
        ax.tick_params(axis="x", length=2, pad=1)  # Shorter ticks, labels closer
        ax.set_xlabel("Time (ms)", labelpad=0.5, fontsize=13)

    ax.margins(0.03)
    ax.set_xlim(x_min, x_max + 1)
    ax.set_ylim(y_min, y_max)

    # ----- Plot dots for lb events -----
    if bugId == "even_idle_cpu":
        dot_size = 10
        ax.set_yticklabels([str(cpu - 4) for cpu in yticks])
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
    parser.add_argument(
        "--time-start",
        type=float,
        default=10.10,
        help="Only plot data from this time onward (in seconds)",
    )
    args = parser.parse_args()

    bugId = args.controller
    log_file_buggy = RESULTS_DIR / f"{bugId}_buggy.log"
    title_buggy = "Buggy"
    log_file_fixed = RESULTS_DIR / f"{bugId}_fixed.log"
    title_fixed = "Fixed"
    output_file = RESULTS_DIR / f"{bugId}.pdf"

    args.time_start = args.time_start if bugId != "even_idle_cpu" else 10.0

    matrix_buggy, cpu_count, time_count, t_buggy, min_buggy, max_buggy = (
        build_nr_running_matrix(log_file_buggy, time_start=args.time_start)
    )
    matrix_fixed, _, _, t_fixed, min_fixed, max_fixed = build_nr_running_matrix(
        log_file_fixed, time_start=args.time_start
    )
    vmin = min(min_buggy, min_fixed)
    vmax = max(max_buggy, max_fixed)

    # Parse LB events for overlays
    lb_events_buggy = parse_lb_events(log_file_buggy, args.time_start)
    lb_events_fixed = parse_lb_events(log_file_fixed, args.time_start)

    fig, axes = plt.subplots(
        2, 1, figsize=(3.5, 2), sharex=False, gridspec_kw={"hspace": 0.3}
    )

    plot_color_matrix_with_lb(
        axes[0],
        matrix_buggy,
        cpu_count,
        time_count,
        vmin,
        vmax,
        t_buggy,
        title_buggy,
        lb_events_buggy,
    )
    plot_color_matrix_with_lb(
        axes[1],
        matrix_fixed,
        cpu_count,
        time_count,
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
