#!/usr/bin/env python3
"""
Script to parse rebalance domain logs and plot overhead over time.
"""

import argparse
import re

import matplotlib.pyplot as plt
import numpy as np

from . import LOGS_DIR, RESULTS_DIR


def parse_log_file(log_file_path, min_time=10.0):
    """
    Parse the log file and extract rebalance domain information.
    
    Args:
        log_file_path: Path to the log file
        min_time: Minimum timestamp to include (default: 10.0)
    
    Returns:
        Dictionary with CPU numbers as keys and (timestamps, latencies) as values
    """
    data = {}
    
    # Regular expression to match lines like:
    # [   10.001000] run_rebalance_domains on CPU 1, latency: 1813 ns
    pattern = r'^\[\s*(\d+\.\d+)\]\s+run_rebalance_domains on CPU (\d+), latency: (\d+) ns'
    
    with open(log_file_path, 'r', encoding='utf-8', errors='ignore') as f:
        for line in f:
            match = re.match(pattern, line)
            if match:
                timestamp = float(match.group(1))
                cpu = int(match.group(2))
                latency_ns = int(match.group(3))
                
                # Only include data after min_time
                if timestamp >= min_time:
                    if cpu not in data:
                        data[cpu] = {'timestamps': [], 'latencies': []}
                    data[cpu]['timestamps'].append(timestamp)
                    data[cpu]['latencies'].append(latency_ns)
    
    return data

def plot_rebalance_comparison(buggy_data, fixed_data, output_file=None, target_cpu=2):
    """
    Plot rebalance overhead comparison between buggy and fixed versions on the same graph.
    
    Args:
        buggy_data: Dictionary with CPU data from buggy log
        fixed_data: Dictionary with CPU data from fixed log
        output_file: Optional path to save the plot
        target_cpu: CPU number to plot (default: 2)
    """
    fig, ax = plt.subplots(figsize=(5, 2))
    
    # Find the minimum timestamp for buggy data
    buggy_min_timestamp = min(min(buggy_data[cpu]['timestamps']) for cpu in buggy_data.keys())
    
    # Plot buggy version data for target CPU
    if target_cpu in buggy_data:
        timestamps = np.array(buggy_data[target_cpu]['timestamps'])
        latencies = np.array(buggy_data[target_cpu]['latencies'])
        
        # Subtract initial timestamp and convert to milliseconds
        timestamps_ms = (timestamps - buggy_min_timestamp) * 1000.0
        
        # Convert latencies to milliseconds for better readability
        latencies_ms = latencies / 1000000.0
        
        ax.scatter(timestamps_ms, latencies_ms, 
                   label=f'Buggy', 
                   alpha=0.6)
    
    # Find the minimum timestamp for fixed data
    fixed_min_timestamp = min(min(fixed_data[cpu]['timestamps']) for cpu in fixed_data.keys())
    
    # Plot fixed version data for target CPU
    if target_cpu in fixed_data:
        timestamps = np.array(fixed_data[target_cpu]['timestamps'])
        latencies = np.array(fixed_data[target_cpu]['latencies'])
        
        # Subtract initial timestamp and convert to milliseconds
        timestamps_ms = (timestamps - fixed_min_timestamp) * 1000.0
        
        # Convert latencies to milliseconds for better readability
        latencies_ms = latencies / 1000000.0
        
        ax.scatter(timestamps_ms, latencies_ms, 
                   label=f'Fixed', 
                   alpha=0.6, s=60, marker='s')
    
    ax.set_xlabel('Time (ms)',)
    ax.set_ylabel('Overhead (ms)')
    ax.set_title(f'2feab24: Rebalance Overhead Comparison')
    ax.legend(loc='best')
    ax.grid(True, alpha=0.3)
    # ax.set_yscale('log')
    
    plt.tight_layout(pad=0.5)
    
    plt.savefig(output_file, dpi=300, bbox_inches='tight')
    print(f"Plot saved to: {output_file}")
    
def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--controller", type=str, default="2feab24")
    args = parser.parse_args()

    bugId = args.controller
    
    # Paths to the log files
    log_dir = LOGS_DIR
    buggy_log = log_dir / f'{bugId}_buggy.log'
    fixed_log = log_dir / f'{bugId}_fixed.log'
    
    print(f"Parsing buggy log: {buggy_log}")
    buggy_data = parse_log_file(buggy_log, min_time=10.0)
    
    if not buggy_data:
        print("No rebalance domain data found in buggy log after time 10.0 seconds.")
        return
    
    print(f"Found data for CPUs in buggy log: {sorted(buggy_data.keys())}")
    
    print(f"\nParsing fixed log: {fixed_log}")
    fixed_data = parse_log_file(fixed_log, min_time=10.0)
    
    if not fixed_data:
        print("No rebalance domain data found in fixed log after time 10.0 seconds.")
        return
    
    print(f"Found data for CPUs in fixed log: {sorted(fixed_data.keys())}")
    
    # Print statistics for CPU 2 only
    target_cpu = 2
    # Generate output filename
    output_file = f"{RESULTS_DIR}/{bugId}.pdf"
    
    # Plot the comparison for CPU 2
    print(f"\nGenerating comparison plot for CPU {target_cpu}...")
    plot_rebalance_comparison(buggy_data, fixed_data, output_file=output_file, target_cpu=target_cpu)

if __name__ == '__main__':
    main()

