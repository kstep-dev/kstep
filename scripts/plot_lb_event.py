#!/usr/bin/env python3
"""
Plot load balancing events from log files
"""

import argparse
import re

import matplotlib.pyplot as plt
from consts import RESULTS_DIR
from matplotlib.ticker import MaxNLocator

init_timestamp = 10.0

all_cpus = [4, 5, 6, 7]


def parse_lb_events(log_file):
    """
    Parse the log file and extract LB events

    Expected format:
    [timestamp] LB cpu weight ignored1 ignored2 ignored3
    Example: [    0.322229] LB 7 2 1 6 7
    Only include those with weight == 4.
    """
    lb_events = []

    # Pattern to match LB lines
    pattern = r"\[\s*([\d\.]+)\]\s+LB\s+(\d+)\s+(\d+)"

    with open(log_file, "r") as f:
        for line in f:
            match = re.search(pattern, line)
            if match:
                timestamp = float(match.group(1))
                if timestamp > init_timestamp:
                    cpu = int(match.group(2))
                    weight = int(match.group(3))
                    # Only keep those with weight == 4
                    if weight != 4:
                        continue
                    # Convert to milliseconds relative to init_timestamp
                    time_ms = (timestamp - init_timestamp) * 1000
                    lb_events.append((time_ms, cpu, weight))

    return lb_events


def get_cpu_colors(cpus):
    """
    Returns a dict mapping each cpu to a distinct color.
    Uses matplotlib's tab10/xkcd colors.
    """
    import matplotlib

    unique_cpus = sorted(set(cpus))
    num_cpus = len(unique_cpus)
    cmap = plt.get_cmap("tab10") if num_cpus <= 10 else plt.get_cmap("tab20")
    color_map = {}
    for idx, cpu in enumerate(unique_cpus):
        color_map[cpu] = cmap(idx % cmap.N)
    return color_map


def plot_lb_events(events_buggy, events_fixed, bugId, output_file):
    """
    Plot LB events with weight == 4, using different color per CPU.
    """
    marker = "o"
    size = 90

    # Get all CPUs to build mapping
    cpus_buggy = [cpu for (time_ms, cpu, weight) in events_buggy if weight == 4]
    cpus_fixed = [cpu for (time_ms, cpu, weight) in events_fixed if weight == 4]
    # all_cpus = sorted(set(cpus_buggy + cpus_fixed))
    cpu_colors = get_cpu_colors(all_cpus)

    fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(6, 3.8))

    # ---- Plot for buggy version ----
    ax1.set_title(f"{bugId} (buggy)")
    ax1.set_ylabel("CPU")
    ax1.set_xticklabels([])
    ax1.grid(True, alpha=0.3)

    times_buggy = [time_ms for (time_ms, cpu, weight) in events_buggy if weight == 4]
    # cpus_buggy already defined above!

    # Plot each CPU's events in its own color
    if times_buggy and cpus_buggy:
        for cpu in sorted(set(cpus_buggy)):
            times = [
                time_ms
                for (time_ms, c, weight) in events_buggy
                if weight == 4 and c == cpu
            ]
            y_cpus = [cpu] * len(times)
            if times:
                ax1.scatter(
                    times,
                    y_cpus,
                    marker=marker,
                    c=[cpu_colors[cpu]],
                    label=f"CPU {cpu}",
                    s=size,
                    lw=1,
                    alpha=0.85,
                    edgecolor="black",
                )
        ax1.legend(loc="upper right", ncol=1, title="CPU")

    # ---- Plot for fixed version ----
    ax2.set_title(f"{bugId} (fixed)")
    ax2.set_ylabel("CPU")
    ax2.set_xlabel("Time (ms)")
    ax2.grid(True, alpha=0.3)

    times_fixed = [time_ms for (time_ms, cpu, weight) in events_fixed if weight == 4]
    # cpus_fixed already defined above!

    if times_fixed and cpus_fixed:
        for cpu in sorted(set(cpus_fixed)):
            times = [
                time_ms
                for (time_ms, c, weight) in events_fixed
                if weight == 4 and c == cpu
            ]
            y_cpus = [cpu] * len(times)
            if times:
                ax2.scatter(
                    times,
                    y_cpus,
                    marker=marker,
                    c=[cpu_colors[cpu]],
                    label=f"CPU {cpu}",
                    s=size,
                    lw=1,
                    alpha=0.85,
                    edgecolor="black",
                )
        ax2.legend(loc="upper right", ncol=1, title="CPU")

    # Set same x/y limits
    all_times = times_buggy + times_fixed
    if all_times:
        ax1.set_xlim(0, max(all_times))
        ax2.set_xlim(0, max(all_times))
    if all_cpus:
        cpu_min, cpu_max = min(all_cpus), max(all_cpus)
        ax1.set_ylim(cpu_min - 0.5, cpu_max + 0.5)
        ax2.set_ylim(cpu_min - 0.5, cpu_max + 0.5)
        ax1.yaxis.set_major_locator(MaxNLocator(integer=True))
        ax2.yaxis.set_major_locator(MaxNLocator(integer=True))

    plt.tight_layout()
    plt.savefig(output_file, dpi=150, bbox_inches="tight")
    print(f"Plot saved to {output_file}")


def main():
    parser = argparse.ArgumentParser(
        description="Plot load balancing events (weight == 4 only)"
    )
    parser.add_argument(
        "--controller", type=str, default="6d7e478", help="Bug ID / controller name"
    )
    args = parser.parse_args()

    bugId = args.controller

    # Paths to the log files
    buggy_log = RESULTS_DIR / f"{bugId}_buggy.log"
    fixed_log = RESULTS_DIR / f"{bugId}_fixed.log"

    output_file = RESULTS_DIR / f"{bugId}.pdf"

    print(f"Parsing buggy log: {buggy_log}")
    events_buggy = parse_lb_events(buggy_log)
    print(f"Found {len(events_buggy)} LB events in buggy version with weight 4")

    print(f"Parsing fixed log: {fixed_log}")
    events_fixed = parse_lb_events(fixed_log)
    print(f"Found {len(events_fixed)} LB events in fixed version with weight 4")

    if not events_buggy and not events_fixed:
        print("No LB events with weight 4 found in log files")
        return

    plot_lb_events(events_buggy, events_fixed, bugId, output_file)


if __name__ == "__main__":
    main()
