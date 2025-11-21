#!/usr/bin/env python3

import argparse
import re
from collections import defaultdict

import matplotlib.pyplot as plt
import numpy as np
from consts import LOGS_DIR, RESULTS_DIR
from matplotlib import colors
from matplotlib.patches import Patch

cmap = plt.cm.tab20
overall_color_matrix = []
pid_to_color = {}
all_cpus = [1, 2]

def build_pid_matrix(filename):
    # Dictionary to store results: {timestamp: {cpu: pid}}
    current_tasks = defaultdict(dict)

    # Regular expression to parse the task lines
    # Format: [timestamp] ... CPU >R PID PPID ...
    task_pattern = re.compile(
        r'\[\s*(\d+\.\d+)\].*?print_tasks:\s+(\d+)\s+>R\s+(\d+)'
    )

    with open(filename, 'r') as f:
        for line in f:
            match = task_pattern.search(line)
            if match:
                timestamp = float(match.group(1))
                cpu = int(match.group(2))
                pid = int(match.group(3))
                current_tasks[timestamp][cpu] = pid

    unique_pids = set()
    # Print results
    for timestamp in sorted(current_tasks.keys()):
        for cpu in sorted(current_tasks[timestamp].keys()):
            pid = current_tasks[timestamp][cpu]
            unique_pids.add(pid)

    unique_pids = sorted(unique_pids)
    pid_to_uid = {pid: i for i, pid in enumerate(unique_pids)}
    timestamps = sorted(current_tasks.keys())

    # Build a 2D array: rows = cpus, cols = timestamps, values = pid
    cpu_idx = {cpu: i for i, cpu in enumerate(all_cpus)}
    cpu_count = 2
    time_count = len(timestamps)
    pid_matrix = np.full((cpu_count, time_count), -1)  # -1 means no task

    for j, ts in enumerate(timestamps):
        for cpu in all_cpus:
            pid = current_tasks[ts].get(cpu, -1)
            pid_matrix[cpu_idx[cpu], j] = pid_to_uid.get(pid, -1)

    # Map each PID to a unique color index
    for pid in unique_pids:
        if pid not in pid_to_color and pid != -1:
            pid_to_color[pid_to_uid[pid]] = len(pid_to_color) + 1
    return pid_matrix, cpu_count, time_count

def build_color_matrix(pid_matrix, cpu_count, time_count):
    color_matrix = np.zeros_like(pid_matrix)
    for i in range(cpu_count):
        for j in range(time_count):
            pid = pid_matrix[i, j]
            color_matrix[i, j] = pid_to_color.get(pid)

    overall_color_matrix.append(color_matrix)

def plot_color_matrix(color_matrix, cpu_count, time_count, vmin, vmax, ax, title): 
    ax.imshow(color_matrix, cmap=cmap, aspect='auto', vmin=vmin, vmax=vmax)

    # Adjust ticks
    ax.set_yticks(np.arange(cpu_count))
    ax.set_yticklabels([f'CPU {cpu}' for cpu in all_cpus])

    if "buggy" in title:
        ax.set_xticks([])
        ax.set_xticklabels([])
        ax.set_xlabel("")
    else:
        xtick_locs = np.arange(0, time_count, 5)
        # Ensure the last tick is included if not already
        if len(xtick_locs) == 0 or xtick_locs[-1] != time_count - 1:
            xtick_locs = np.append(xtick_locs, time_count - 1)

        ax.set_xticks(xtick_locs)
        ax.set_xticklabels([f"{i}" for i in xtick_locs], rotation=45)
        ax.set_xlabel("Time (ms)")
    
    ax.set_title(title)

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--controller", type=str, default="aa3ee4f")
    args = parser.parse_args()

    bugId = args.controller
    log_file_buggy  = f"{LOGS_DIR}/{bugId}_buggy.log"
    title_buggy = f"{bugId} (buggy)"

    log_file_fixed = f"{LOGS_DIR}/{bugId}_fixed.log"
    title_fixed = f"{bugId} (fixed)"

    output_file = f"{RESULTS_DIR}/{bugId}.pdf"

    start_timestamp = 10.000000

    fig, axes = plt.subplots(2, 1, figsize=(8, 3), constrained_layout=True)
    pid_matrix_buggy, cpu_count_buggy, time_count_buggy = build_pid_matrix(log_file_buggy)
    pid_matrix_fixed, cpu_count_fixed, time_count_fixed = build_pid_matrix(log_file_fixed)
    pid_to_color[-1] = 0

    build_color_matrix(pid_matrix_buggy, cpu_count_buggy, time_count_buggy)
    build_color_matrix(pid_matrix_fixed, cpu_count_fixed, time_count_fixed)

    vmin = min([overall_color_matrix[i].min() for i in range(len(overall_color_matrix))])
    vmax = max([overall_color_matrix[i].max() for i in range(len(overall_color_matrix))])

    plot_color_matrix(overall_color_matrix[0], cpu_count_buggy, time_count_buggy, vmin, vmax, axes[0], title_buggy)
    plot_color_matrix(overall_color_matrix[1], cpu_count_fixed, time_count_fixed, vmin, vmax, axes[1], title_fixed)

    legend_elements = []

    norm = colors.Normalize(vmin=min([overall_color_matrix[i].min() for i in range(len(overall_color_matrix))]), 
                            vmax=max([overall_color_matrix[i].max() for i in range(len(overall_color_matrix))]))
    for pid, color_idx in pid_to_color.items():
        if not any((overall_color_matrix[i] == color_idx).any() for i in range(len(overall_color_matrix))):
            continue
        
        if pid == -1:
            label = "No task"
        else:
            label = f"PID {pid}"
        legend_elements.append(Patch(facecolor=cmap(norm(color_idx)), label=label))

    fig.legend(handles=legend_elements, 
                bbox_to_anchor=(1.01, 0.5), 
                loc='center left', 
                borderaxespad=0.,
                ncol=1)

    plt.tight_layout()
    plt.savefig(output_file, bbox_inches='tight', pad_inches=0)
