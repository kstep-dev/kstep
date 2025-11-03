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

bugId = "aa3ee4f"
log_file  = f"{PROJ_DIR}/data/{bugId}_buggy.txt"
title_buggy = f"{bugId} (buggy)"

log_file2 = f"{PROJ_DIR}/data/{bugId}_fixed.txt"
title_fixed = f"{bugId} (fixed)"

output_file = f"{PROJ_DIR}/plot/{bugId}.pdf"

start_timestamp = 10.000000

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

    # Print results
    for timestamp in sorted(current_tasks.keys()):
        print(f"Timestamp: {timestamp:.6f}")
        for cpu in sorted(current_tasks[timestamp].keys()):
            pid = current_tasks[timestamp][cpu]
            print(f"  CPU {cpu}: PID {pid}")
        print()


    # Get all cpus and sorted timestamps
    # all_cpus = set()
    # for ts in current_tasks.values():
        # all_cpus.update(ts.keys())
    # all_cpus = sorted(all_cpus)
    timestamps = sorted(current_tasks.keys())

    # Build a 2D array: rows = cpus, cols = timestamps, values = pid
    cpu_idx = {cpu: i for i, cpu in enumerate(all_cpus)}
    cpu_count = 2
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
    for pid in sorted_pids:
        if pid not in pid_to_color and pid != -1:
            pid_to_color[pid] = len(pid_to_color) + 1
    return pid_matrix, cpu_count, time_count, all_cpus

def build_color_matrix(pid_matrix, cpu_count, time_count, all_cpus, ax, title):
    color_matrix = np.zeros_like(pid_matrix)
    for i in range(cpu_count):
        for j in range(time_count):
            pid = pid_matrix[i, j]
            color_matrix[i, j] = pid_to_color.get(pid)

    print(color_matrix)
    overall_color_matrix.append(color_matrix)
    
    ax.imshow(color_matrix, cmap=cmap, aspect='auto')

    # Adjust ticks
    ax.set_yticks(np.arange(cpu_count))
    ax.set_yticklabels([f'CPU {cpu}' for cpu in all_cpus])

    xtick_locs = np.arange(0, time_count, 5)
    # Ensure the last tick is included if not already
    if len(xtick_locs) == 0 or xtick_locs[-1] != time_count - 1:
        xtick_locs = np.append(xtick_locs, time_count - 1)

    ax.set_xticks(xtick_locs)
    ax.set_xticklabels([f"{i}" for i in xtick_locs], rotation=45)

    ax.set_xlabel("Time (ms)")
    ax.set_title(title)


fig, axes = plt.subplots(2, 1, figsize=(8, 3), constrained_layout=True)
pid_matrix_buggy, cpu_count_buggy, time_count_buggy, all_cpus_buggy = build_pid_matrix(log_file)
pid_matrix_fixed, cpu_count_fixed, time_count_fixed, all_cpus_fixed = build_pid_matrix(log_file2)
pid_to_color[-1] = 0

build_color_matrix(pid_matrix_buggy, cpu_count_buggy, time_count_buggy, all_cpus_buggy, axes[0], title_buggy)
build_color_matrix(pid_matrix_fixed, cpu_count_fixed, time_count_fixed, all_cpus_fixed, axes[1], title_fixed)

legend_elements = []

norm = colors.Normalize(vmin=min([overall_color_matrix[i].min() for i in range(len(overall_color_matrix))]), 
                        vmax=max([overall_color_matrix[i].max() for i in range(len(overall_color_matrix))]))
for pid, color_idx in pid_to_color.items():
    print("--", color_idx, pid)
    if not any((overall_color_matrix[i] == color_idx).any() for i in range(len(overall_color_matrix))):
        continue
    
    if pid == -1:
        label = "No task"
    else:
        label = f"PID {pid}"
    legend_elements.append(Patch(facecolor=cmap(norm(color_idx)), label=label))
# legend_elements.append(Patch(facecolor=cmap(norm(pid_to_color[-1])), label="No task"))
fig.legend(handles=legend_elements, 
            bbox_to_anchor=(1.01, 0.5), 
            loc='center left', 
            borderaxespad=0.,
            ncol=1)

plt.tight_layout()
plt.savefig(output_file, bbox_inches='tight', pad_inches=0)
