#!/usr/bin/env python3
"""
Plot load balancing events from log files
"""

import re
import matplotlib.pyplot as plt
import sys
import os
import argparse
import math

from matplotlib.ticker import MaxNLocator

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), '..')))

from scripts import PROJ_DIR, LOGS_DIR

init_timestamp = 10.0

def parse_lb_events(log_file):
    """
    Parse the log file and extract LB events
    
    Expected format:
    [timestamp] LB cpu weight ignored1 ignored2 ignored3
    Example: [    0.322229] LB 7 2 1 6 7
    Ignore those with weight 8.
    """
    lb_events = []
    
    # Pattern to match LB lines
    pattern = r'\[\s*(\d+\.\d+)\]\s+LB\s+(\d+)\s+(\d+)'
    
    with open(log_file, 'r') as f:
        for line in f:
            match = re.search(pattern, line)
            if match:
                timestamp = float(match.group(1))
                if timestamp >= init_timestamp:
                    cpu = int(match.group(2))
                    weight = int(match.group(3))
                    # Ignore those with weight 8
                    if weight == 8:
                        continue
                    # Convert to milliseconds relative to init_timestamp
                    time_ms = (timestamp - init_timestamp) * 1000
                    lb_events.append((time_ms, cpu, weight))
    
    return lb_events

def plot_lb_events(events_buggy, events_fixed, bugId, output_file):
    """
    Plot LB events with different markers for different weights.
    Use the same marker and visual style for both buggy and fixed subplots.
    """
    # Marker and color settings for all weights (shared for both subplots)
    weight_styles = {
        2: {'marker': 's', 'facecolor': 'none', 'edgecolor': '#28878a', 'label': 'Sched Domain (2 CPUs)', 'size': 65},  # hollow square
        4: {'marker': 'o', 'facecolor': '#b23c1a', 'edgecolor': '#672613', 'label': 'Sched Domain (4 CPUs)', 'size': 90},  # deep red dot
        16: {'marker': 'D', 'facecolor': '#FFA07A', 'edgecolor': '#a06343', 'label': 'Sched Domain (16 CPUs)', 'size': 65},
        32: {'marker': 'v', 'facecolor': '#98D8C8', 'edgecolor': '#52856e', 'label': 'Sched Domain (32 CPUs)', 'size': 65},
    }
    fallback_style = {'marker': 'x', 'facecolor': 'gray', 'edgecolor': 'black', 'label': 'Other', 'size': 65}

    fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(6, 3.8))

    # ---- Plot for buggy version ----
    ax1.set_title(f'{bugId} (buggy)')
    ax1.set_ylabel('CPU')
    ax1.set_xticklabels([])
    # ax1.set_xlabel('Time (ms)')
    ax1.grid(True, alpha=0.3)

    weight_groups_buggy = {}
    for time_ms, cpu, weight in events_buggy:
        if weight == 8:
            continue
        if weight not in weight_groups_buggy:
            weight_groups_buggy[weight] = {'times': [], 'cpus': []}
        weight_groups_buggy[weight]['times'].append(time_ms)
        weight_groups_buggy[weight]['cpus'].append(cpu)

    for weight in sorted(weight_groups_buggy.keys()):
        style = weight_styles.get(weight, fallback_style)
        ax1.scatter(weight_groups_buggy[weight]['times'],
                    weight_groups_buggy[weight]['cpus'],
                    marker=style['marker'],
                    facecolor=style['facecolor'],
                    edgecolor=style['edgecolor'],
                    label=style['label'],
                    s=style['size'],
                    lw=1.5 if style.get('facecolor', None) == 'none' else 1,
                    alpha=0.85,
        )

    ax1.legend(loc='upper right', ncol = 1)

    # ---- Plot for fixed version ----
    ax2.set_title(f'{bugId} (fixed)')
    ax2.set_ylabel('CPU')
    ax2.set_xlabel('Time (ms)')
    ax2.grid(True, alpha=0.3)

    weight_groups_fixed = {}
    for time_ms, cpu, weight in events_fixed:
        if weight == 8:
            continue
        if weight not in weight_groups_fixed:
            weight_groups_fixed[weight] = {'times': [], 'cpus': []}
        weight_groups_fixed[weight]['times'].append(time_ms)
        weight_groups_fixed[weight]['cpus'].append(cpu)

    for weight in sorted(weight_groups_fixed.keys()):
        style = weight_styles.get(weight, fallback_style)
        ax2.scatter(weight_groups_fixed[weight]['times'],
                    weight_groups_fixed[weight]['cpus'],
                    marker=style['marker'],
                    facecolor=style['facecolor'],
                    edgecolor=style['edgecolor'],
                    label=style['label'],
                    s=style['size'],
                    lw=1.5 if style.get('facecolor', None) == 'none' else 1,
                    alpha=0.85,
        )

    # ax2.legend(loc='upper right', ncol = 1)

    # Set same x/y limits
    all_times = [e[0] for e in events_buggy if e[2] != 8] + [e[0] for e in events_fixed if e[2] != 8]
    if all_times:
        ax1.set_xlim(0, max(all_times))
        ax2.set_xlim(0, max(all_times))

    all_cpus = [e[1] for e in events_buggy if e[2] != 8] + [e[1] for e in events_fixed if e[2] != 8]
    if all_cpus:
        cpu_min, cpu_max = min(all_cpus), max(all_cpus)
        ax1.set_ylim(cpu_min - 0.5, cpu_max + 0.5)
        ax2.set_ylim(cpu_min - 0.5, cpu_max + 0.5)
        # Set yticks to int only
        ax1.yaxis.set_major_locator(MaxNLocator(integer=True))
        ax2.yaxis.set_major_locator(MaxNLocator(integer=True))

    plt.tight_layout()
    plt.savefig(output_file, dpi=150, bbox_inches='tight')
    print(f"Plot saved to {output_file}")

def main():
    parser = argparse.ArgumentParser(description='Plot load balancing events')
    parser.add_argument("--controller", type=str, default="6d7e478",
                       help="Bug ID / controller name")
    args = parser.parse_args()

    bugId = args.controller
    
    # Paths to the log files
    log_dir = LOGS_DIR
    buggy_log = log_dir / f'{bugId}_buggy.log'
    fixed_log = log_dir / f'{bugId}_fixed.log'

    output_file = f"{PROJ_DIR}/plot/{bugId}.pdf"
    
    print(f"Parsing buggy log: {buggy_log}")
    events_buggy = parse_lb_events(buggy_log)
    print(f"Found {len(events_buggy)} LB events in buggy version (before filtering weight 8)")
    
    print(f"Parsing fixed log: {fixed_log}")
    events_fixed = parse_lb_events(fixed_log)
    print(f"Found {len(events_fixed)} LB events in fixed version (before filtering weight 8)")
    
    if not events_buggy and not events_fixed:
        print("No LB events found in log files")
        return
    
    plot_lb_events(events_buggy, events_fixed, bugId, output_file)

if __name__ == '__main__':
    main()