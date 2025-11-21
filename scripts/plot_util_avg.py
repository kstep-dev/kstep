#!/usr/bin/env python3
"""
Plot util_avg for CPU 2 over time from log files
"""

import argparse
import re

import matplotlib.pyplot as plt

from . import LOGS_DIR, RESULTS_DIR

init_timestamp = 10.0
def parse_log_file(log_file):
    """
    Parse the log file and extract timestamp and avg_util for CPU 2
    
    Expected format:
    [timestamp] ... print_tasks: - CPU 2 running=X, switches=Y, avg_load=Z, avg_util=VALUE
    """
    timestamps = []
    util_avg_values = []
    
    # Pattern to match lines with CPU 2 and avg_util
    pattern = r'\[\s*(\d+\.\d+)\].*CPU 2.*avg_util=(\d+)'
    
    with open(log_file, 'r') as f:
        for line in f:
            match = re.search(pattern, line)
            if match and float(match.group(1)) >= init_timestamp:
                timestamp = (float(match.group(1)) - init_timestamp) * 1000
                util_avg = int(match.group(2))
                timestamps.append(timestamp)
                util_avg_values.append(util_avg)
    print(len(timestamps))
    return timestamps, util_avg_values

def plot_util_avg(timestamps_buggy, util_avg_values_buggy, 
                  timestamps_fixed, util_avg_values_fixed, bugId, output_file):
    """
    Plot util_avg over time in two subplots
    """
    fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(6, 3))
    
    # Plot buggy version in first subplot
    ax1.plot(timestamps_buggy, util_avg_values_buggy, linewidth=3, linestyle='-', color='#A72703')
    # ax1.set_xlabel('Time (ms)')
    ax1.set_ylabel('util_avg')
    ax1.set_title(f'{bugId} - Buggy Version')
    ax1.grid(True, alpha=0.3)
    ax1.set_xticklabels([])
    ax1.set_xlim(0, max(max(timestamps_buggy), max(timestamps_fixed)))
    
    # Plot fixed version in second subplot
    ax2.plot(timestamps_fixed, util_avg_values_fixed, linewidth=3, linestyle='-', color='#BBC863')
    ax2.set_xlabel('Time (ms)')
    ax2.set_ylabel('util_avg')
    ax2.set_title(f'{bugId} - Fixed Version')
    ax2.grid(True, alpha=0.3)
    ax2.set_xlim(0, max(max(timestamps_buggy), max(timestamps_fixed)))
    
    plt.tight_layout()
    plt.savefig(output_file, dpi=150, bbox_inches='tight')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--controller", type=str, default="17e3e88")
    args = parser.parse_args()

    bugId = args.controller
    
    # Paths to the log files
    log_dir = LOGS_DIR
    buggy_log = log_dir / f'{bugId}_buggy.log'
    fixed_log = log_dir / f'{bugId}_fixed.log'

    output_file = f"{RESULTS_DIR}/{bugId}.pdf"
    
    print(f"Parsing log file: {buggy_log}")
    timestamps_buggy, util_avg_values_buggy = parse_log_file(buggy_log)
    timestamps_fixed, util_avg_values_fixed = parse_log_file(fixed_log)

    plot_util_avg(timestamps_buggy, util_avg_values_buggy, 
                  timestamps_fixed, util_avg_values_fixed, bugId, output_file)

if __name__ == '__main__':
    main()

