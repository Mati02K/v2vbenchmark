#!/usr/bin/env python3
"""
RAFT Intersection Benchmark Comparison Plotter
Compares WAVE vs UDP performance across different vehicle counts.

Usage:
  python3 plot_comparison.py           # Uses raft_*, raftwave_* (intersection_4/8/16)
  python3 plot_comparison.py --simple  # Uses simple_udp_*, simple_raftwave_* (simple_intersection)
"""

import argparse
import json
import os
import sys
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
        'messages_sent': [],
        'messages_received': [],
        'delivery_ratio': [],      # heuristic: received / (sent * (N-1)), 1 = perfect
        'estimated_loss_rate': [],  # 1 - delivery_ratio
        'fallback_rate': [],   # fraction of vehicles per run that used fallback
    }

    for run_data in runs_data:
        if not run_data:
            continue

        num_vehicles = len(run_data)

        # RAFT decision time = sum(commit - propose) = sum of per-vehicle raft_decision_time (each leader has sum for entries they proposed)
        raft_vehicles = [v for v in run_data if v.get('coordination_method') != 'fallback']
        rd_per_veh = [v['durations_ms'].get('raft_decision_time', 0) for v in raft_vehicles if v['durations_ms'].get('raft_decision_time', 0) > 0]
        if rd_per_veh:
            metrics['raft_decision_time'].append(sum(rd_per_veh))

        # Total intersection time (legacy: first stop to last pass)
        stopped_times = [v['timestamps_ms']['stopped'] for v in run_data]
        passed_times = [v['timestamps_ms']['passed'] for v in run_data]
        if stopped_times and passed_times:
            total_time = max(passed_times) - min(stopped_times)
            metrics['total_intersection_time'].append(total_time)

        # Throughput: num_RAFT / (last_RAFT_passed - first_RAFT_passed), exclude fallback
        raft_passed = [v['timestamps_ms']['passed'] for v in raft_vehicles if v['timestamps_ms'].get('passed', 0) > 0]
        if len(raft_passed) >= 1:
            raft_time_ms = max(raft_passed) - min(raft_passed)
            if raft_time_ms > 0:
                n_count = len(raft_vehicles)
                metrics['throughput'].append(n_count / (raft_time_ms / 1000.0))

        # Fallback rate: fraction of vehicles that used fallback this run
        fallback_count = sum(1 for v in run_data if v.get('coordination_method') == 'fallback')
        metrics['fallback_rate'].append(fallback_count / num_vehicles if num_vehicles > 0 else 0)

        # Per-vehicle metrics
        total_sent = sum(vehicle['messages']['sent'] for vehicle in run_data)
        total_received = sum(vehicle['messages']['received'] for vehicle in run_data)
        metrics['messages_sent'].append(total_sent)
        metrics['messages_received'].append(total_received)

        # Heuristic for message loss (assumes broadcast-heavy: 1 send -> (N-1) ideal receives)
        # delivery_ratio = actual / expected; <1 implies loss
        N = num_vehicles
        expected_received = total_sent * (N - 1) if N > 1 else total_sent
        if expected_received > 0:
            ratio = min(1.0, total_received / expected_received)
            metrics['delivery_ratio'].append(ratio)
            metrics['estimated_loss_rate'].append(1.0 - ratio)
        for vehicle in run_data:
            metrics['wait_time'].append(vehicle['durations_ms']['total_wait_time'])
            metrics['transit_time'].append(vehicle['durations_ms']['transit_time'])

    # Calculate mean and std for each metric
    result = {}
    for key, values in metrics.items():
        if values:
            result[key] = {'mean': np.mean(values), 'std': np.std(values)}
        else:
            result[key] = {'mean': 0, 'std': 0}

    return result

def main():
    parser = argparse.ArgumentParser(description='RAFT Intersection Benchmark Comparison')
    parser.add_argument('--simple', action='store_true',
                        help='Use simple_intersection results (simple_udp_*, simple_raftwave_*)')
    args = parser.parse_args()

    vehicle_counts = [4, 8, 16]
    protocols = ['wave', 'udp']

    if args.simple:
        folder_prefix = {'wave': 'simple_raftwave', 'udp': 'simple_udp'}
        output_prefix = 'simple_'
        title_note = ''
    else:
        folder_prefix = {'wave': 'raftwave', 'udp': 'raft'}
        output_prefix = ''
        title_note = ''

    # Colors for each protocol
    colors = {'wave': '#9b59b6', 'udp': '#3498db'}  # Purple for WAVE, Blue for UDP
    labels = {'wave': 'WAVE (IEEE 802.11p / ITS-G5)', 'udp': 'UDP (IEEE 802.11a)'}
    
    # Collect data
    data = {protocol: {} for protocol in protocols}
    
    for protocol in protocols:
        prefix = folder_prefix[protocol]
        for vc in vehicle_counts:
            # For simple mode: use exact name. For intersection: prefer _fixed2 > _report > plain
            if args.simple:
                result_name = f"{prefix}_{vc}veh"
            else:
                for suffix in ['_fixed2', '_report', '']:
                    candidate = f"{prefix}_{vc}veh{suffix}"
                    if os.path.isdir(os.path.join(RESULTS_DIR, candidate)):
                        result_name = candidate
                        break
                else:
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
                        raw = f.read()
                    d = json.loads(raw)
                    if d:
                        runs.append(d)
                except json.JSONDecodeError:
                    # Repair truncated JSON (missing closing ])
                    try:
                        s = raw.rstrip()
                        if s and not s.endswith(']'):
                            if s.endswith('}'):
                                d = json.loads(s + '\n]')
                            else:
                                d = json.loads(s.rstrip(',') + '\n]')
                            if d:
                                runs.append(d)
                    except Exception:
                        pass
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
                        'fallback_rate': stats.get('fallback_rate', {'mean': 0, 'std': 0}),
                        'messages_sent': stats.get('messages_sent', {'mean': 0, 'std': 0}),
                        'messages_received': stats.get('messages_received', {'mean': 0, 'std': 0}),
                        'delivery_ratio': stats.get('delivery_ratio', {'mean': 0, 'std': 0}),
                        'estimated_loss_rate': stats.get('estimated_loss_rate', {'mean': 0, 'std': 0})
                    }
    
    # Create figure: 1 column (Throughput only)
    fig, ax_throughput = plt.subplots(1, 1, figsize=(7, 5))
    axes = [ax_throughput]
    fig.suptitle(
        f'RAFT Intersection Coordination: WAVE (IEEE 802.11p/ITS-G5) vs UDP (IEEE 802.11a){title_note}\n'
        'Industry-Realistic PHY: α=2.75 NLOS, LogNormal Shadowing σ=4dB, Tx=20mW, 6 Mbps',
        fontsize=14, fontweight='bold'
    )

    # Metrics to plot: Throughput only
    # (metric_key, title, ylabel, col, scale) - scale optional, default 1
    plot_configs = [
        ('throughput', 'System Throughput', 'Vehicles/second', 0),
    ]
    
    bar_width = 0.35
    x = np.arange(len(vehicle_counts))
    
    for config in plot_configs:
        metric = config[0]
        title = config[1]
        ylabel = config[2]
        col = config[3]
        scale = config[4] if len(config) > 4 else 1.0
        ax = axes[col]
        
        for i, protocol in enumerate(protocols):
            means = []
            stds = []
            for vc in vehicle_counts:
                if vc in data[protocol] and metric in data[protocol][vc]:
                    means.append(data[protocol][vc][metric]['mean'] * scale)
                    stds.append(data[protocol][vc][metric]['std'] * scale)
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
    output_file = os.path.join(RESULTS_DIR, f'{output_prefix}wave_vs_udp_comparison.png')
    plt.savefig(output_file, dpi=150, bbox_inches='tight')
    print(f"Comparison plot saved to: {output_file}")
    
    # Also save as PDF for higher quality
    pdf_file = os.path.join(RESULTS_DIR, f'{output_prefix}wave_vs_udp_comparison.pdf')
    plt.savefig(pdf_file, bbox_inches='tight')
    print(f"PDF version saved to: {pdf_file}")
    
    plt.close()

    # --- Separate Fallback Rate plot ---
    fig_fb, ax_fb = plt.subplots(figsize=(8, 5))
    bar_width = 0.35
    x = np.arange(len(vehicle_counts))

    for i, protocol in enumerate(protocols):
        means = []
        stds = []
        for vc in vehicle_counts:
            if vc in data[protocol] and 'fallback_rate' in data[protocol][vc]:
                means.append(data[protocol][vc]['fallback_rate']['mean'] * 100)
                stds.append(data[protocol][vc]['fallback_rate']['std'] * 100)
            else:
                means.append(0)
                stds.append(0)

        offset = (i - 0.5) * bar_width
        bars = ax_fb.bar(x + offset, means, bar_width, yerr=stds,
                        label=labels[protocol], color=colors[protocol],
                        alpha=0.8, capsize=5, edgecolor='black', linewidth=0.5)

        for bar, mean in zip(bars, means):
            if mean > 0:
                height = bar.get_height()
                ax_fb.text(bar.get_x() + bar.get_width()/2., height,
                          f'{mean:.1f}%', ha='center', va='bottom', fontsize=9, fontweight='bold')

    ax_fb.set_xlabel('Number of Vehicles', fontsize=11, fontweight='bold')
    ax_fb.set_ylabel('Fallback Rate (%)', fontsize=11, fontweight='bold')
    ax_fb.set_title('Fallback Rate: Vehicles Passing Without RAFT Consensus', fontsize=12, fontweight='bold')
    ax_fb.set_xticks(x)
    ax_fb.set_xticklabels([f'{vc} veh' for vc in vehicle_counts])
    ax_fb.legend(loc='upper left')
    ax_fb.grid(True, alpha=0.3, axis='y')
    ax_fb.set_ylim(bottom=0)

    plt.tight_layout()
    fallback_png = os.path.join(RESULTS_DIR, 'fallbacks.png')
    fallback_pdf = os.path.join(RESULTS_DIR, 'fallbacks.pdf')
    fig_fb.savefig(fallback_png, dpi=150, bbox_inches='tight')
    fig_fb.savefig(fallback_pdf, bbox_inches='tight')
    plt.close(fig_fb)
    print(f"Fallback plot saved to: {fallback_png}")

    # --- Messages Sent/Received/Loss plot ---
    fig_msg, axes_msg = plt.subplots(1, 3, figsize=(14, 5))
    fig_msg.suptitle(
        f'RAFT Intersection: Messages Sent, Received & Estimated Loss{title_note}\n'
        'Industry-Realistic PHY: α=2.75 NLOS, LogNormal Shadowing σ=4dB, Tx=20mW, 6 Mbps',
        fontsize=14, fontweight='bold'
    )
    bar_width = 0.35
    x = np.arange(len(vehicle_counts))

    msg_plot_configs = [
        ('messages_sent', 'Messages Sent (total per run)', 'Messages', 1),
        ('messages_received', 'Messages Received (total per run)', 'Messages', 1),
        ('estimated_loss_rate', 'Est. Message Loss Rate (heuristic)', 'Loss Rate (%)', 100),
    ]
    for col, (metric, title, ylabel, scale) in enumerate(msg_plot_configs):
        ax = axes_msg[col]
        for i, protocol in enumerate(protocols):
            means = []
            stds = []
            for vc in vehicle_counts:
                if vc in data[protocol] and metric in data[protocol][vc]:
                    means.append(data[protocol][vc][metric]['mean'] * scale)
                    stds.append(data[protocol][vc][metric]['std'] * scale)
                else:
                    means.append(0)
                    stds.append(0)

            offset = (i - 0.5) * bar_width
            bars = ax.bar(x + offset, means, bar_width, yerr=stds,
                         label=labels[protocol], color=colors[protocol],
                         alpha=0.8, capsize=5, edgecolor='black', linewidth=0.5)

            for bar, mean in zip(bars, means):
                if mean > 0 or (col == 2 and scale == 100):
                    fmt = f'{mean:.1f}%' if scale == 100 else f'{mean:.0f}'
                    ax.text(bar.get_x() + bar.get_width()/2., bar.get_height(),
                            fmt, ha='center', va='bottom', fontsize=8, fontweight='bold')

        ax.set_xlabel('Number of Vehicles', fontsize=11, fontweight='bold')
        ax.set_ylabel(ylabel, fontsize=11, fontweight='bold')
        ax.set_title(title, fontsize=12, fontweight='bold')
        ax.set_xticks(x)
        ax.set_xticklabels([f'{vc} veh' for vc in vehicle_counts])
        ax.legend(loc='upper left')
        ax.grid(True, alpha=0.3, axis='y')
        ax.set_ylim(bottom=0)

    plt.tight_layout()
    messages_png = os.path.join(RESULTS_DIR, 'messages.png')
    messages_pdf = os.path.join(RESULTS_DIR, 'messages.pdf')
    fig_msg.savefig(messages_png, dpi=150, bbox_inches='tight')
    fig_msg.savefig(messages_pdf, bbox_inches='tight')
    plt.close(fig_msg)
    print(f"Messages plot saved to: {messages_png}")

    # --- CDF: Total Wait Time per vehicle ---
    # Collect raw per-vehicle wait times (one value per vehicle per run)
    raw_wait = {protocol: {} for protocol in protocols}
    for protocol in protocols:
        prefix = folder_prefix[protocol]
        for vc in vehicle_counts:
            result_name = f"{prefix}_{vc}veh" if args.simple else None
            if not args.simple:
                for suffix in ['_fixed2', '_report', '']:
                    candidate = f"{prefix}_{vc}veh{suffix}"
                    if os.path.isdir(os.path.join(RESULTS_DIR, candidate)):
                        result_name = candidate
                        break
                else:
                    result_name = f"{prefix}_{vc}veh"
            result_dir = os.path.join(RESULTS_DIR, result_name)
            times = []
            run_num = 1
            while True:
                json_file = os.path.join(result_dir, f'run_{run_num}', 'raft_results.json')
                if not os.path.exists(json_file):
                    break
                try:
                    with open(json_file) as f:
                        raw = f.read()
                    d = json.loads(raw)
                except json.JSONDecodeError:
                    try:
                        s = raw.rstrip()
                        d = json.loads((s + '\n]') if s.endswith('}') else (s.rstrip(',') + '\n]'))
                    except Exception:
                        run_num += 1
                        continue
                except Exception:
                    run_num += 1
                    continue
                for v in d:
                    if v.get('coordination_method') == 'fallback':
                        continue  # exclude fallback vehicles (consistent with throughput)
                    wt = v['durations_ms'].get('total_wait_time', 0)
                    if wt > 0:
                        times.append(wt / 1000.0)  # convert to seconds
                run_num += 1
            raw_wait[protocol][vc] = times

    fig_cdf, axes_cdf = plt.subplots(1, len(vehicle_counts), figsize=(14, 5), sharey=True)
    fig_cdf.suptitle(
        f'CDF of Total Wait Time per Vehicle (RAFT-coordinated only){title_note}\n'
        'Time from vehicle first stopping to crossing the intersection',
        fontsize=13, fontweight='bold'
    )
    linestyles = {'wave': '-', 'udp': '--'}

    for col, vc in enumerate(vehicle_counts):
        ax = axes_cdf[col]
        for protocol in protocols:
            times = raw_wait[protocol].get(vc, [])
            if not times:
                continue
            sorted_t = np.sort(times)
            cdf = np.arange(1, len(sorted_t) + 1) / len(sorted_t)
            ax.plot(sorted_t, cdf,
                    color=colors[protocol],
                    linestyle=linestyles[protocol],
                    linewidth=2,
                    label=f"{labels[protocol]} (n={len(times)})")
        ax.set_title(f'{vc} Vehicles', fontsize=11, fontweight='bold')
        ax.set_xlabel('Total Wait Time (s)', fontsize=10)
        if col == 0:
            ax.set_ylabel('CDF', fontsize=10)
        ax.set_ylim(0, 1.05)
        ax.set_xlim(left=0)
        ax.legend(fontsize=8)
        ax.grid(True, alpha=0.3)

    plt.tight_layout()
    cdf_png = os.path.join(RESULTS_DIR, f'{output_prefix}cdf_wait_time.png')
    cdf_pdf = os.path.join(RESULTS_DIR, f'{output_prefix}cdf_wait_time.pdf')
    fig_cdf.savefig(cdf_png, dpi=150, bbox_inches='tight')
    fig_cdf.savefig(cdf_pdf, bbox_inches='tight')
    plt.close(fig_cdf)
    print(f"CDF plot saved to: {cdf_png}")

    # --- Cluster Size per Election plot ---
    # One row, 3 subplots (4/8/16 veh).
    # X-axis: WAVE elections grouped left, UDP elections grouped right.
    # Each bar = one run where a RAFT election happened; Y = vehicles in that election.
    # Only runs with at least one vehicle doing RAFT get a bar.

    # Collect per-run cluster sizes (only runs with actual RAFT elections)
    election_sizes = {protocol: {} for protocol in protocols}
    for protocol in protocols:
        prefix = folder_prefix[protocol]
        for vc in vehicle_counts:
            result_name = f"{prefix}_{vc}veh" if args.simple else None
            if not args.simple:
                for suffix in ['_fixed2', '_report', '']:
                    candidate = f"{prefix}_{vc}veh{suffix}"
                    if os.path.isdir(os.path.join(RESULTS_DIR, candidate)):
                        result_name = candidate
                        break
                else:
                    result_name = f"{prefix}_{vc}veh"
            result_dir = os.path.join(RESULTS_DIR, result_name)
            sizes = []
            run_num = 1
            while True:
                json_file = os.path.join(result_dir, f'run_{run_num}', 'raft_results.json')
                if not os.path.exists(json_file):
                    break
                try:
                    with open(json_file) as f:
                        raw = f.read()
                    d = json.loads(raw)
                except json.JSONDecodeError:
                    try:
                        s = raw.rstrip()
                        d = json.loads((s + '\n]') if s.endswith('}') else (s.rstrip(',') + '\n]'))
                    except Exception:
                        run_num += 1
                        continue
                except Exception:
                    run_num += 1
                    continue
                # Only include runs where a RAFT election actually happened
                cluster_size = sum(1 for v in d if v.get('coordination_method') == 'raft')
                if cluster_size > 0:
                    sizes.append(cluster_size)
                run_num += 1
            election_sizes[protocol][vc] = sizes

    fig_cl, axes_cl = plt.subplots(1, len(vehicle_counts), figsize=(14, 5))
    fig_cl.suptitle(
        f'RAFT Election Cluster Size per Run{title_note}\n'
        'Each bar = one intersection event; height = vehicles that participated in that election',
        fontsize=12, fontweight='bold'
    )

    bar_w = 0.6
    gap   = 1.5  # gap between WAVE group and UDP group

    for col, vc in enumerate(vehicle_counts):
        ax = axes_cl[col]

        wave_sizes = election_sizes['wave'].get(vc, [])
        udp_sizes  = election_sizes['udp'].get(vc, [])

        # WAVE bars: positions 0..n_wave-1
        # UDP bars: positions n_wave+gap .. n_wave+gap+n_udp-1
        wave_x = np.arange(len(wave_sizes), dtype=float)
        udp_x  = np.arange(len(udp_sizes),  dtype=float) + len(wave_sizes) + gap

        if len(wave_sizes):
            ax.bar(wave_x, wave_sizes, width=bar_w,
                   color=colors['wave'], alpha=0.85, edgecolor='black', linewidth=0.5,
                   label=labels['wave'])
            for x, s in zip(wave_x, wave_sizes):
                ax.text(x, s + 0.1, str(s), ha='center', va='bottom', fontsize=8)

        if len(udp_sizes):
            ax.bar(udp_x, udp_sizes, width=bar_w,
                   color=colors['udp'], alpha=0.85, edgecolor='black', linewidth=0.5,
                   label=labels['udp'])
            for x, s in zip(udp_x, udp_sizes):
                ax.text(x, s + 0.1, str(s), ha='center', va='bottom', fontsize=8)

        # Group labels on x-axis
        all_x    = list(wave_x) + list(udp_x)
        all_lbl  = [f'W{i+1}' for i in range(len(wave_sizes))] + \
                   [f'U{i+1}' for i in range(len(udp_sizes))]
        ax.set_xticks(all_x)
        ax.set_xticklabels(all_lbl, fontsize=7)

        # Underline group labels with a bracket / annotation
        if len(wave_sizes):
            mid_w = float(np.mean(wave_x))
            ax.annotate('WAVE', xy=(mid_w, -0.12), xycoords=('data', 'axes fraction'),
                        ha='center', va='top', fontsize=9, fontweight='bold',
                        color=colors['wave'])
        if len(udp_sizes):
            mid_u = float(np.mean(udp_x))
            ax.annotate('UDP', xy=(mid_u, -0.12), xycoords=('data', 'axes fraction'),
                        ha='center', va='top', fontsize=9, fontweight='bold',
                        color=colors['udp'])

        ax.axhline(vc, color='gray', linestyle='--', linewidth=1, label=f'Max ({vc})')
        ax.set_title(f'{vc} Vehicles', fontsize=11, fontweight='bold')
        ax.set_ylim(0, vc + 2)
        ax.set_yticks(range(0, vc + 2))
        if col == 0:
            ax.set_ylabel('Vehicles in RAFT Election', fontsize=10)
        ax.grid(True, alpha=0.3, axis='y')
        ax.legend(fontsize=7, loc='lower right')

    plt.tight_layout()
    cluster_png = os.path.join(RESULTS_DIR, f'{output_prefix}cluster_leader.png')
    cluster_pdf = os.path.join(RESULTS_DIR, f'{output_prefix}cluster_leader.pdf')
    fig_cl.savefig(cluster_png, dpi=150, bbox_inches='tight')
    fig_cl.savefig(cluster_pdf, bbox_inches='tight')
    plt.close(fig_cl)
    print(f"Cluster/leader plot saved to: {cluster_png}")

    # Print summary table
    print("\n" + "="*70)
    print("SUMMARY: WAVE (IEEE 802.11p/ITS-G5) vs UDP (IEEE 802.11a)")
    print("="*70)
    print(f"{'Vehicles':<10} {'Protocol':<22} {'RAFT Decision (ms)':<20} {'Throughput (veh/s)':<20}")
    print("-"*70)

    for vc in vehicle_counts:
        for protocol in protocols:
            if vc in data[protocol]:
                d = data[protocol][vc]
                proto_label = labels[protocol]
                raft = d.get('raft_decision_time', {}).get('mean', 0)
                tp = d.get('throughput', {}).get('mean', 0)
                print(f"{vc:<10} {proto_label:<22} {raft:<20.1f} {tp:<20.3f}")
        print("-"*70)

if __name__ == '__main__':
    main()
