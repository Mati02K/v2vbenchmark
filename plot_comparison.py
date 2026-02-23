#!/usr/bin/env python3
"""
RAFT Intersection Benchmark Comparison Plotter
Compares WAVE vs UDP performance across different vehicle counts.
"""

import json
import os
import matplotlib.pyplot as plt
import numpy as np

RESULTS_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'results')

def load_aggregate_stats(protocol, vehicle_count):
    """Load aggregate statistics for a given protocol and vehicle count."""
    result_name = f"{protocol}_{vehicle_count}veh"
    stats_file = os.path.join(RESULTS_DIR, result_name, 'aggregate_stats.json')
    
    if os.path.exists(stats_file):
        with open(stats_file, 'r') as f:
            return json.load(f)
    return None

def load_raw_runs(protocol, vehicle_count):
    """Load raw data from all runs for more detailed analysis."""
    result_name = f"{protocol}_{vehicle_count}veh"
    result_dir = os.path.join(RESULTS_DIR, result_name)
    
    all_data = []
    run_num = 1
    while True:
        json_file = os.path.join(result_dir, f'run_{run_num}', 'raft_results.json')
        if not os.path.exists(json_file):
            break
        try:
            with open(json_file, 'r') as f:
                data = json.load(f)
                if data:
                    all_data.append(data)
        except:
            pass
        run_num += 1
    return all_data

def calculate_metrics_from_runs(runs_data):
    """Calculate metrics from raw run data."""
    metrics = {
        'raft_decision_time': [],
        'total_intersection_time': [],
        'throughput': [],
        'wait_time': [],
        'transit_time': [],
        'messages_sent': []
    }
    
    for run_data in runs_data:
        if not run_data:
            continue
            
        num_vehicles = len(run_data)
        
        # RAFT decision time: time from first stopped to latest commit
        raft_commits = [v['timestamps_ms'].get('order_committed', 0) for v in run_data 
                       if v.get('coordination_method') != 'fallback' and v['timestamps_ms'].get('order_committed', 0) > 0]
        stopped_times = [v['timestamps_ms']['stopped'] for v in run_data]
        
        if raft_commits and stopped_times:
            raft_decision = max(raft_commits) - min(stopped_times)
            if raft_decision > 0:
                metrics['raft_decision_time'].append(raft_decision)
        
        # Total intersection time
        passed_times = [v['timestamps_ms']['passed'] for v in run_data]
        if stopped_times and passed_times:
            total_time = max(passed_times) - min(stopped_times)
            metrics['total_intersection_time'].append(total_time)
            
            # Throughput
            if total_time > 0:
                metrics['throughput'].append(num_vehicles / (total_time / 1000.0))
        
        # Per-vehicle metrics
        for vehicle in run_data:
            metrics['wait_time'].append(vehicle['durations_ms']['total_wait_time'])
            metrics['transit_time'].append(vehicle['durations_ms']['transit_time'])
            metrics['messages_sent'].append(vehicle['messages']['sent'])
    
    # Calculate mean and std for each metric
    result = {}
    for key, values in metrics.items():
        if values:
            result[key] = {'mean': np.mean(values), 'std': np.std(values)}
        else:
            result[key] = {'mean': 0, 'std': 0}
    
    return result

def main():
    vehicle_counts = [4, 8, 16, 32]
    protocols = ['wave', 'udp']

    # Folder-name prefix: results are stored as raftwave_Nveh / raft_Nveh
    folder_prefix = {'wave': 'raftwave', 'udp': 'raft'}

    # Colors for each protocol
    colors = {'wave': '#9b59b6', 'udp': '#3498db'}  # Purple for WAVE, Blue for UDP
    labels = {'wave': 'WAVE (IEEE 802.11p / ITS-G5)', 'udp': 'UDP (IEEE 802.11a)'}
    
    # Collect data
    data = {protocol: {} for protocol in protocols}
    
    for protocol in protocols:
        prefix = folder_prefix[protocol]
        for vc in vehicle_counts:
            # Override result_name used inside helpers by patching the folder directly
            result_name = f"{prefix}_{vc}veh"
            # Load raw runs directly (bypass helper that uses wrong prefix)
            result_dir = os.path.join(RESULTS_DIR, result_name)
            runs = []
            run_num = 1
            while True:
                json_file = os.path.join(result_dir, f'run_{run_num}', 'raft_results.json')
                if not os.path.exists(json_file):
                    break
                try:
                    with open(json_file, 'r') as f:
                        d = json.load(f)
                        if d:
                            runs.append(d)
                except Exception:
                    pass
                run_num += 1

            if runs:
                data[protocol][vc] = calculate_metrics_from_runs(runs)
            else:
                # Fallback: aggregate_stats.json
                stats_file = os.path.join(result_dir, 'aggregate_stats.json')
                if os.path.exists(stats_file):
                    with open(stats_file, 'r') as f:
                        stats = json.load(f)
                    data[protocol][vc] = {
                        'raft_decision_time': stats.get('raft_decision_time', {'mean': 0, 'std': 0}),
                        'total_intersection_time': stats.get('total_intersection_time', {'mean': 0, 'std': 0}),
                        'throughput': stats.get('throughput', {'mean': 0, 'std': 0}),
                        'wait_time': stats.get('total_wait_time', {'mean': 0, 'std': 0}),
                        'transit_time': stats.get('transit_time', {'mean': 0, 'std': 0}),
                        'messages_sent': stats.get('messages_sent', {'mean': 0, 'std': 0})
                    }
    
    # Create figure with 2 rows x 3 columns
    fig, axes = plt.subplots(2, 3, figsize=(18, 11))
    fig.suptitle(
        'RAFT Intersection Coordination: WAVE (IEEE 802.11p/ITS-G5) vs UDP (IEEE 802.11a)\n'
        'Industry-Realistic PHY: α=2.75 NLOS, LogNormal Shadowing σ=4dB, Tx=20mW, 6 Mbps',
        fontsize=14, fontweight='bold'
    )
    
    # Metrics to plot
    plot_configs = [
        ('raft_decision_time', 'RAFT Decision Time', 'Time (ms)', 0, 0),
        ('total_intersection_time', 'Total Intersection Time', 'Time (ms)', 0, 1),
        ('throughput', 'System Throughput', 'Vehicles/second', 0, 2),
        ('wait_time', 'Average Wait Time', 'Time (ms)', 1, 0),
        ('transit_time', 'Average Transit Time', 'Time (ms)', 1, 1),
        ('messages_sent', 'Messages Sent per Vehicle', 'Count', 1, 2)
    ]
    
    bar_width = 0.35
    x = np.arange(len(vehicle_counts))
    
    for metric, title, ylabel, row, col in plot_configs:
        ax = axes[row, col]
        
        for i, protocol in enumerate(protocols):
            means = []
            stds = []
            for vc in vehicle_counts:
                if vc in data[protocol] and metric in data[protocol][vc]:
                    means.append(data[protocol][vc][metric]['mean'])
                    stds.append(data[protocol][vc][metric]['std'])
                else:
                    means.append(0)
                    stds.append(0)
            
            offset = (i - 0.5) * bar_width
            bars = ax.bar(x + offset, means, bar_width, yerr=stds, 
                         label=labels[protocol], color=colors[protocol], 
                         alpha=0.8, capsize=5, edgecolor='black', linewidth=0.5)
            
            # Add value labels on bars
            for j, (bar, mean) in enumerate(zip(bars, means)):
                if mean > 0:
                    height = bar.get_height()
                    if metric == 'throughput':
                        ax.text(bar.get_x() + bar.get_width()/2., height,
                               f'{mean:.3f}', ha='center', va='bottom', fontsize=8, fontweight='bold')
                    else:
                        ax.text(bar.get_x() + bar.get_width()/2., height,
                               f'{mean:.0f}', ha='center', va='bottom', fontsize=8, fontweight='bold')
        
        ax.set_xlabel('Number of Vehicles', fontsize=11, fontweight='bold')
        ax.set_ylabel(ylabel, fontsize=11, fontweight='bold')
        ax.set_title(title, fontsize=12, fontweight='bold')
        ax.set_xticks(x)
        ax.set_xticklabels([f'{vc} veh' for vc in vehicle_counts])
        ax.legend(loc='upper left')
        ax.grid(True, alpha=0.3, axis='y')
        ax.set_ylim(bottom=0)
    
    plt.tight_layout()
    
    # Save the plot
    output_file = os.path.join(RESULTS_DIR, 'wave_vs_udp_comparison.png')
    plt.savefig(output_file, dpi=150, bbox_inches='tight')
    print(f"Comparison plot saved to: {output_file}")
    
    # Also save as PDF for higher quality
    pdf_file = os.path.join(RESULTS_DIR, 'wave_vs_udp_comparison.pdf')
    plt.savefig(pdf_file, bbox_inches='tight')
    print(f"PDF version saved to: {pdf_file}")
    
    plt.close()
    
    # Print summary table
    print("\n" + "="*90)
    print("SUMMARY TABLE: WAVE (IEEE 802.11p/ITS-G5) vs UDP (IEEE 802.11a) — Industry-Realistic PHY")
    print("="*90)
    print(f"{'Vehicles':<10} {'Protocol':<22} {'RAFT (ms)':<18} {'Total (ms)':<18} {'Throughput':<14} {'Wait (ms)':<12}")
    print("-"*90)

    for vc in vehicle_counts:
        for protocol in protocols:
            if vc in data[protocol]:
                d = data[protocol][vc]
                proto_label = labels[protocol]
                raft = d.get('raft_decision_time', {}).get('mean', 0)
                total = d.get('total_intersection_time', {}).get('mean', 0)
                tp = d.get('throughput', {}).get('mean', 0)
                wait = d.get('wait_time', {}).get('mean', 0)
                print(f"{vc:<10} {proto_label:<22} {raft:<18.1f} {total:<18.1f} {tp:<14.3f} {wait:<12.1f}")
        print("-"*90)

if __name__ == '__main__':
    main()
