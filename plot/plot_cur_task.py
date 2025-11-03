import re
from collections import defaultdict
import sys
import os

import matplotlib.pyplot as plt
import numpy as np
from matplotlib.patches import Patch
from matplotlib import colors

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), '..')))

from scripts import PROJ_DIR

log_file  = f"{PROJ_DIR}/data/log_bug.txt"
title = "bbce3de (buggy)"

log_file2 = f"{PROJ_DIR}/data/log_fix.txt"
title2 = "bbce3de (fixed)"

output_file = f"{PROJ_DIR}/plot/bbce3de.pdf"

start_timestamp = 10.000000

def plot_cur_task(log_file, title, ax, legend):
    # Dictionary to store results: {timestamp: {cpu: pid}}
    current_tasks = defaultdict(dict)

    # Regular expression to parse the task lines
    # Format: [timestamp] ... CPU >R PID PPID ...
    task_pattern = re.compile(
        r'\[\s*(\d+\.\d+)\].*?print_tasks:\s+(\d+)\s+>R\s+(\d+)'
    )

    with open(log_file, 'r') as f:
        for line in f:
            match = task_pattern.search(line)
            if match:
                timestamp = float(match.group(1))
                cpu = int(match.group(2))
                pid = int(match.group(3))
                current_tasks[timestamp][cpu] = pid

    # Print results
    for timestamp in sorted(current_tasks.keys()):
        print(f"Timestamp: {timestamp:.6f}")
        for cpu in sorted(current_tasks[timestamp].keys()):
            pid = current_tasks[timestamp][cpu]
            print(f"  CPU {cpu}: PID {pid}")
        print()


    # Get all cpus and sorted timestamps
    all_cpus = set()
    for ts in current_tasks.values():
        all_cpus.update(ts.keys())
    all_cpus = sorted(all_cpus)
    timestamps = sorted(current_tasks.keys())

    # Build a 2D array: rows = cpus, cols = timestamps, values = pid
    cpu_idx = {cpu: i for i, cpu in enumerate(all_cpus)}
    cpu_count = len(all_cpus)
    time_count = len(timestamps)
    pid_matrix = np.full((cpu_count, time_count), -1)  # -1 means no task

    for j, ts in enumerate(timestamps):
        for cpu in all_cpus:
            pid = current_tasks[ts].get(cpu, -1)
            pid_matrix[cpu_idx[cpu], j] = pid

    print(pid_matrix)
    # Map each PID to a unique color index
    all_pids = set(pid_matrix.flatten())
    all_pids.discard(-1)
    sorted_pids = sorted(all_pids)
    pid_to_color = {pid: pid for pid in sorted_pids}  # color 0 is for "no task"
    color_matrix = np.zeros_like(pid_matrix)
    for i in range(cpu_count):
        for j in range(time_count):
            pid = pid_matrix[i, j]
            color_matrix[i, j] = pid_to_color.get(pid, 0)

    ax.imshow(color_matrix, cmap=plt.cm.tab20, aspect='auto')

    # Adjust ticks
    ax.set_yticks(np.arange(cpu_count))
    ax.set_yticklabels([f'CPU {cpu}' for cpu in all_cpus])

    xtick_locs = np.arange(0, time_count, 5)
    # Ensure the last tick is included if not already
    if len(xtick_locs) == 0 or xtick_locs[-1] != time_count - 1:
        xtick_locs = np.append(xtick_locs, time_count - 1)

    ax.set_xticks(xtick_locs)
    ax.set_xticklabels([f"{i}" for i in xtick_locs], rotation=45)

    if not legend:
        ax.set_xlabel("Time (ms)")
    else:
        ax.set_xlabel("")
    ax.set_title(title)

    # Build a list of legend elements whose colors match pid_to_color assignments
    if legend:
        legend_elements = []
        cmap = plt.cm.tab20
        norm = colors.Normalize(vmin=0, vmax=max(pid_to_color.values()))
        for pid in sorted_pids:
            color_idx = pid_to_color[pid]
            print(color_idx, pid)
            legend_elements.append(Patch(facecolor=cmap(norm(color_idx)), label=f"PID {pid}"))
        fig.legend(handles=legend_elements, 
                  bbox_to_anchor=(1.01, 0.5), 
                  loc='center left', 
                  borderaxespad=0.,
                  ncol=1)

fig, axes = plt.subplots(2, 1, figsize=(8, 3), constrained_layout=True)
plot_cur_task(log_file, title, axes[0], True)
plot_cur_task(log_file2, title2, axes[1], False)
plt.tight_layout()
plt.savefig(output_file, bbox_inches='tight', pad_inches=0)
