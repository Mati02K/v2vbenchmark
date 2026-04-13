#!/usr/bin/env python3
"""
RAFT Intersection Benchmark Plotter
Compares WAVE vs UDP performance across vehicle counts.

Usage:
  python3 plot_comparison.py --simple                          # laneLeaders plots
  python3 plot_comparison.py --simple --mode allVehicles       # allVehicles plots
  python3 plot_comparison.py --simple --compare-modes          # side-by-side comparison
"""

import argparse
import json
import os
import numpy as np
import matplotlib.pyplot as plt
from matplotlib.patches import Patch

RESULTS_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'results')
VEHICLE_COUNTS = [4, 8, 16]
PROTOCOLS = ['wave', 'udp']
PROTOCOL_PREFIXES = {'wave': 'simple_raftwave', 'udp': 'simple_udp'}
PROTOCOL_LABELS = {'wave': 'WAVE (802.11p)', 'udp': 'UDP (802.11a)'}
PROTOCOL_COLORS = {'wave': '#9b59b6', 'udp': '#3498db'}


# ============================================================
# Data Loading
# ============================================================

def loadRunsForMode(protocolPrefix, vc, mode):
    """Load raw JSON run data for a protocol, vehicle count, and cluster mode.
    Handles truncated JSON files (missing closing bracket)."""
    resultName = f"{protocolPrefix}_{vc}veh_{mode}"
    resultDir = os.path.join(RESULTS_DIR, resultName)
    runs = []
    runNum = 1
    while True:
        jsonFile = os.path.join(resultDir, f'run_{runNum}', 'raft_results.json')
        if not os.path.exists(jsonFile):
            break
        try:
            with open(jsonFile) as f:
                raw = f.read()
            d = json.loads(raw)
            if d:
                runs.append(d)
        except json.JSONDecodeError:
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
        runNum += 1
    return runs


def calculateMetrics(runsData):
    """Calculate aggregate metrics (mean/std) from a list of per-run vehicle arrays."""
    metrics = {
        'raftDecisionTime': [],
        'throughput': [],
        'waitTime': [],
        'msgSentPerVehicle': [],
        'msgReceivedPerVehicle': [],
        'fallbackRate': [],
    }

    for runData in runsData:
        if not runData:
            continue
        numVehicles = len(runData)

        # RAFT decision time: sum across non-fallback vehicles
        raftVehicles = [v for v in runData if v.get('coordination_method') != 'fallback']
        rdPerVeh = [v['durations_ms'].get('raft_decision_time', 0)
                    for v in raftVehicles
                    if v['durations_ms'].get('raft_decision_time', 0) > 0]
        if rdPerVeh:
            metrics['raftDecisionTime'].append(sum(rdPerVeh))

        # Throughput: raft vehicles / time span
        raftPassed = [v['timestamps_ms']['passed'] for v in raftVehicles
                      if v['timestamps_ms'].get('passed', 0) > 0]
        if len(raftPassed) >= 1:
            raftTimeMs = max(raftPassed) - min(raftPassed)
            if raftTimeMs > 0:
                metrics['throughput'].append(len(raftVehicles) / (raftTimeMs / 1000.0))

        # Fallback rate per run
        fallbackCount = sum(1 for v in runData if v.get('coordination_method') == 'fallback')
        metrics['fallbackRate'].append(fallbackCount / numVehicles if numVehicles > 0 else 0)

        # Average messages per vehicle
        totalSent = sum(v['messages']['sent'] for v in runData)
        totalRecv = sum(v['messages']['received'] for v in runData)
        metrics['msgSentPerVehicle'].append(totalSent / numVehicles if numVehicles > 0 else 0)
        metrics['msgReceivedPerVehicle'].append(totalRecv / numVehicles if numVehicles > 0 else 0)

        # Per-vehicle wait times
        for v in runData:
            metrics['waitTime'].append(v['durations_ms']['total_wait_time'])

    result = {}
    for key, vals in metrics.items():
        if vals:
            result[key] = {'mean': float(np.mean(vals)), 'std': float(np.std(vals))}
        else:
            result[key] = {'mean': 0, 'std': 0}
    return result


def loadDataForMode(mode):
    """Load and compute metrics for all protocol x vehicle-count combinations."""
    data = {}
    for protocol in PROTOCOLS:
        for vc in VEHICLE_COUNTS:
            runs = loadRunsForMode(PROTOCOL_PREFIXES[protocol], vc, mode)
            if runs:
                data[(protocol, vc)] = calculateMetrics(runs)
    return data


# ============================================================
# Plot Helpers
# ============================================================

def savePlot(fig, path):
    """Save figure as PNG (150 dpi) and close it."""
    fig.savefig(path, dpi=150, bbox_inches='tight')
    plt.close(fig)
    print(f"  Saved: {os.path.basename(path)}")


def getMetricValues(data, protocol, metric, scale=1.0):
    """Extract mean/std arrays for a metric across all vehicle counts."""
    means, stds = [], []
    for vc in VEHICLE_COUNTS:
        key = (protocol, vc)
        if key in data and metric in data[key]:
            means.append(data[key][metric]['mean'] * scale)
            stds.append(data[key][metric]['std'] * scale)
        else:
            means.append(0)
            stds.append(0)
    return means, stds


# ============================================================
# Single-Mode Plots (WAVE vs UDP for one cluster mode)
# ============================================================

def plotThroughput(data, mode):
    """Bar chart: system throughput (veh/s) for WAVE vs UDP."""
    fig, ax = plt.subplots(figsize=(8, 5))
    fig.suptitle(f'System Throughput [{mode}]\nWAVE vs UDP across vehicle counts',
                 fontsize=13, fontweight='bold')

    barWidth = 0.35
    x = np.arange(len(VEHICLE_COUNTS))

    for i, protocol in enumerate(PROTOCOLS):
        means, stds = getMetricValues(data, protocol, 'throughput')
        offset = (i - 0.5) * barWidth
        bars = ax.bar(x + offset, means, barWidth, yerr=stds,
                      label=PROTOCOL_LABELS[protocol], color=PROTOCOL_COLORS[protocol],
                      alpha=0.8, capsize=5, edgecolor='black', linewidth=0.5)
        for bar, mean in zip(bars, means):
            if mean > 0:
                ax.text(bar.get_x() + bar.get_width() / 2., bar.get_height(),
                        f'{mean:.3f}', ha='center', va='bottom', fontsize=8, fontweight='bold')

    ax.set_xlabel('Number of Vehicles', fontsize=11, fontweight='bold')
    ax.set_ylabel('Vehicles/second', fontsize=11, fontweight='bold')
    ax.set_xticks(x)
    ax.set_xticklabels([f'{vc} veh' for vc in VEHICLE_COUNTS])
    ax.legend(bbox_to_anchor=(0.5, -0.15), loc='upper center', ncol=2, fontsize=9)
    ax.grid(True, alpha=0.3, axis='y')
    ax.set_ylim(bottom=0)
    plt.tight_layout(rect=[0, 0.05, 1, 1])

    savePlot(fig, os.path.join(RESULTS_DIR, f'throughput_{mode}.png'))


def plotFallbacks(data, mode):
    """Bar chart: fallback rate (%) for WAVE vs UDP."""
    fig, ax = plt.subplots(figsize=(8, 5))
    fig.suptitle(f'Fallback Rate [{mode}]\nVehicles passing without RAFT consensus',
                 fontsize=13, fontweight='bold')

    barWidth = 0.35
    x = np.arange(len(VEHICLE_COUNTS))

    for i, protocol in enumerate(PROTOCOLS):
        means, stds = getMetricValues(data, protocol, 'fallbackRate', scale=100)
        offset = (i - 0.5) * barWidth
        bars = ax.bar(x + offset, means, barWidth, yerr=stds,
                      label=PROTOCOL_LABELS[protocol], color=PROTOCOL_COLORS[protocol],
                      alpha=0.8, capsize=5, edgecolor='black', linewidth=0.5)
        for bar, mean in zip(bars, means):
            if mean > 0:
                ax.text(bar.get_x() + bar.get_width() / 2., bar.get_height(),
                        f'{mean:.1f}%', ha='center', va='bottom', fontsize=9, fontweight='bold')

    ax.set_xlabel('Number of Vehicles', fontsize=11, fontweight='bold')
    ax.set_ylabel('Fallback Rate (%)', fontsize=11, fontweight='bold')
    ax.set_xticks(x)
    ax.set_xticklabels([f'{vc} veh' for vc in VEHICLE_COUNTS])
    ax.legend(bbox_to_anchor=(0.5, -0.15), loc='upper center', ncol=2, fontsize=9)
    ax.grid(True, alpha=0.3, axis='y')
    ax.set_ylim(bottom=0)
    plt.tight_layout(rect=[0, 0.05, 1, 1])

    savePlot(fig, os.path.join(RESULTS_DIR, f'fallbacks_{mode}.png'))


def plotMessages(data, mode):
    """2-panel bar chart: avg messages sent and received per vehicle."""
    fig, axes = plt.subplots(1, 2, figsize=(12, 5))
    fig.suptitle(f'Average Messages per Vehicle [{mode}]\nWAVE vs UDP across vehicle counts',
                 fontsize=13, fontweight='bold')

    barWidth = 0.35
    x = np.arange(len(VEHICLE_COUNTS))
    configs = [
        ('msgSentPerVehicle', 'Avg Sent per Vehicle'),
        ('msgReceivedPerVehicle', 'Avg Received per Vehicle'),
    ]

    for col, (metric, title) in enumerate(configs):
        ax = axes[col]
        for i, protocol in enumerate(PROTOCOLS):
            means, stds = getMetricValues(data, protocol, metric)
            offset = (i - 0.5) * barWidth
            bars = ax.bar(x + offset, means, barWidth, yerr=stds,
                          label=PROTOCOL_LABELS[protocol], color=PROTOCOL_COLORS[protocol],
                          alpha=0.8, capsize=5, edgecolor='black', linewidth=0.5)
            for bar, mean in zip(bars, means):
                if mean > 0:
                    ax.text(bar.get_x() + bar.get_width() / 2., bar.get_height(),
                            f'{mean:.0f}', ha='center', va='bottom', fontsize=8, fontweight='bold')
        ax.set_xlabel('Number of Vehicles', fontsize=10, fontweight='bold')
        ax.set_ylabel('Messages', fontsize=10, fontweight='bold')
        ax.set_title(title, fontsize=11, fontweight='bold')
        ax.set_xticks(x)
        ax.set_xticklabels([f'{vc} veh' for vc in VEHICLE_COUNTS])
        ax.legend(bbox_to_anchor=(0.5, -0.18), loc='upper center', ncol=2, fontsize=8)
        ax.grid(True, alpha=0.3, axis='y')
        ax.set_ylim(bottom=0)

    plt.tight_layout(rect=[0, 0.05, 1, 1])
    savePlot(fig, os.path.join(RESULTS_DIR, f'messages_{mode}.png'))


def plotCdfWaitTime(mode):
    """CDF of total wait time per vehicle, one subplot per vehicle count."""
    fig, axes = plt.subplots(1, len(VEHICLE_COUNTS), figsize=(14, 5), sharey=True)
    fig.suptitle(f'CDF of Total Wait Time [{mode}]\n'
                 f'Time from stopping to crossing (RAFT only)',
                 fontsize=13, fontweight='bold')

    linestyles = {'wave': '-', 'udp': '--'}

    for col, vc in enumerate(VEHICLE_COUNTS):
        ax = axes[col]
        for protocol in PROTOCOLS:
            runs = loadRunsForMode(PROTOCOL_PREFIXES[protocol], vc, mode)
            times = []
            for runData in runs:
                for v in runData:
                    if v.get('coordination_method') == 'fallback':
                        continue
                    wt = v['durations_ms'].get('total_wait_time', 0)
                    if wt > 0:
                        times.append(wt / 1000.0)
            if not times:
                continue
            sortedT = np.sort(times)
            cdf = np.arange(1, len(sortedT) + 1) / len(sortedT)
            ax.plot(sortedT, cdf, color=PROTOCOL_COLORS[protocol],
                    linestyle=linestyles[protocol], linewidth=2,
                    label=f"{PROTOCOL_LABELS[protocol]} (n={len(times)})")

        ax.set_title(f'{vc} Vehicles', fontsize=11, fontweight='bold')
        ax.set_xlabel('Total Wait Time (s)', fontsize=10)
        if col == 0:
            ax.set_ylabel('CDF', fontsize=10)
        ax.set_ylim(0, 1.05)
        ax.set_xlim(left=0)
        ax.legend(fontsize=8, loc='lower right')
        ax.grid(True, alpha=0.3)

    plt.tight_layout()
    savePlot(fig, os.path.join(RESULTS_DIR, f'cdf_{mode}.png'))


def plotAmbulanceWaitTime(mode):
    """Bar chart: ambulance vs normal vehicle wait time, one subplot per vehicle count."""
    ambulanceIds = {4: 3, 8: 7, 16: 14}

    ambWait = {p: {} for p in PROTOCOLS}
    normWait = {p: {} for p in PROTOCOLS}

    for protocol in PROTOCOLS:
        for vc in VEHICLE_COUNTS:
            ambId = ambulanceIds.get(vc)
            runs = loadRunsForMode(PROTOCOL_PREFIXES[protocol], vc, mode)
            ambTimes, normTimes = [], []
            for runData in runs:
                for v in runData:
                    if v.get('coordination_method') == 'fallback':
                        continue
                    passedMs = v['timestamps_ms'].get('passed', 0)
                    stoppedMs = v['timestamps_ms'].get('stopped', 0)
                    if passedMs <= 0:
                        continue
                    startedMs = v['timestamps_ms'].get('started_moving', 0)
                    if stoppedMs > 0:
                        latencyS = (passedMs - stoppedMs) / 1000.0
                    elif startedMs > 0:
                        latencyS = (passedMs - startedMs) / 1000.0
                    else:
                        continue
                    if v['vehicle_id'] == ambId:
                        ambTimes.append(latencyS)
                    else:
                        normTimes.append(latencyS)
            ambWait[protocol][vc] = np.mean(ambTimes) if ambTimes else 0.0
            normWait[protocol][vc] = np.mean(normTimes) if normTimes else 0.0

    fig, axes = plt.subplots(1, len(VEHICLE_COUNTS), figsize=(14, 5), sharey=False)
    fig.suptitle(f'Wait Time: Ambulance vs Normal [{mode}]\nStopped to passed (seconds)',
                 fontsize=13, fontweight='bold')

    barW = 0.3
    groupGap = 0.9
    ambColor = '#e74c3c'
    normColor = '#95a5a6'

    for col, vc in enumerate(VEHICLE_COUNTS):
        ax = axes[col]
        xWave = np.array([0.0, barW])
        xUdp = xWave + barW + groupGap

        wAmb = ambWait['wave'].get(vc, 0)
        wNorm = normWait['wave'].get(vc, 0)
        uAmb = ambWait['udp'].get(vc, 0)
        uNorm = normWait['udp'].get(vc, 0)

        ax.bar(xWave[0], wAmb, width=barW, color=ambColor, alpha=0.9,
               edgecolor='black', linewidth=0.7)
        ax.bar(xWave[1], wNorm, width=barW, color=normColor, alpha=0.9,
               edgecolor='black', linewidth=0.7)
        ax.bar(xUdp[0], uAmb, width=barW, color=ambColor, alpha=0.9,
               edgecolor='black', linewidth=0.7, hatch='//')
        ax.bar(xUdp[1], uNorm, width=barW, color=normColor, alpha=0.9,
               edgecolor='black', linewidth=0.7, hatch='//')

        for xPos, val in [(xWave[0], wAmb), (xWave[1], wNorm),
                          (xUdp[0], uAmb), (xUdp[1], uNorm)]:
            if val > 0:
                ax.text(xPos, val + 0.03, f'{val:.1f}s', ha='center', va='bottom',
                        fontsize=8, fontweight='bold')

        midWave = float(np.mean(xWave))
        midUdp = float(np.mean(xUdp))
        ax.annotate('WAVE', xy=(midWave, -0.09), xycoords=('data', 'axes fraction'),
                    ha='center', va='top', fontsize=9, fontweight='bold',
                    color=PROTOCOL_COLORS['wave'])
        ax.annotate('UDP', xy=(midUdp, -0.09), xycoords=('data', 'axes fraction'),
                    ha='center', va='top', fontsize=9, fontweight='bold',
                    color=PROTOCOL_COLORS['udp'])

        ax.set_xticks([])
        ax.set_title(f'{vc} Vehicles\n(ambulance=V{ambulanceIds[vc]})',
                     fontsize=11, fontweight='bold')
        if col == 0:
            ax.set_ylabel('Mean Crossing Time (s)', fontsize=10)
        ax.set_xlim(-barW * 0.8, float(xUdp[1]) + barW * 1.2)
        maxVal = max(wAmb, wNorm, uAmb, uNorm, 0.1)
        ax.set_ylim(0, maxVal * 1.25 + 0.5)
        ax.grid(True, alpha=0.3, axis='y')

        if col == 0:
            legendElements = [
                Patch(facecolor=ambColor, edgecolor='black', label='Ambulance'),
                Patch(facecolor=normColor, edgecolor='black', label='Normal vehicles'),
                Patch(facecolor='white', edgecolor='black', label='WAVE (solid)'),
                Patch(facecolor='white', edgecolor='black', hatch='//', label='UDP (hatched)'),
            ]
            ax.legend(handles=legendElements, fontsize=8, loc='upper right')

    plt.tight_layout()
    savePlot(fig, os.path.join(RESULTS_DIR, f'ambulance_{mode}.png'))


# ============================================================
# Compare-Mode Plots (laneLeaders vs allVehicles side by side)
# ============================================================

COMPARE_MODES = ['laneLeaders', 'allVehicles']
COMPARE_SERIES = [(p, m) for p in PROTOCOLS for m in COMPARE_MODES]
COMPARE_COLORS = {
    ('wave', 'laneLeaders'): '#9b59b6',
    ('wave', 'allVehicles'): '#9b59b6',
    ('udp', 'laneLeaders'): '#3498db',
    ('udp', 'allVehicles'): '#3498db',
}
COMPARE_HATCHES = {
    ('wave', 'laneLeaders'): '',
    ('wave', 'allVehicles'): '//',
    ('udp', 'laneLeaders'): '',
    ('udp', 'allVehicles'): '//',
}
COMPARE_LABELS = {
    ('wave', 'laneLeaders'): 'WAVE / Lane Leaders',
    ('wave', 'allVehicles'): 'WAVE / All Vehicles',
    ('udp', 'laneLeaders'): 'UDP / Lane Leaders',
    ('udp', 'allVehicles'): 'UDP / All Vehicles',
}


def loadCompareData():
    """Load metrics for all protocol x mode x vehicle-count combinations."""
    data = {}
    for protocol in PROTOCOLS:
        prefix = PROTOCOL_PREFIXES[protocol]
        for mode in COMPARE_MODES:
            for vc in VEHICLE_COUNTS:
                runs = loadRunsForMode(prefix, vc, mode)
                if runs:
                    data[(protocol, mode, vc)] = calculateMetrics(runs)
    return data


def getCompareValues(data, protocol, mode, metric, scale=1.0):
    """Extract mean/std arrays for a metric across vehicle counts in compare mode."""
    means, stds = [], []
    for vc in VEHICLE_COUNTS:
        key = (protocol, mode, vc)
        if key in data and metric in data[key]:
            means.append(data[key][metric]['mean'] * scale)
            stds.append(data[key][metric]['std'] * scale)
        else:
            means.append(0)
            stds.append(0)
    return means, stds


def plotCompareBars(data, metric, title, ylabel, outputName, scale=1.0, valueFmt='{:.3f}'):
    """Generic 4-series bar chart for compare mode."""
    fig, ax = plt.subplots(figsize=(10, 6))
    fig.suptitle(title, fontsize=13, fontweight='bold')

    barWidth = 0.18
    x = np.arange(len(VEHICLE_COUNTS))

    for i, (protocol, mode) in enumerate(COMPARE_SERIES):
        means, stds = getCompareValues(data, protocol, mode, metric, scale)
        offset = (i - 1.5) * barWidth
        bars = ax.bar(x + offset, means, barWidth, yerr=stds,
                      label=COMPARE_LABELS[(protocol, mode)],
                      color=COMPARE_COLORS[(protocol, mode)],
                      hatch=COMPARE_HATCHES[(protocol, mode)],
                      alpha=0.85, capsize=4, edgecolor='black', linewidth=0.5)
        for bar, mean in zip(bars, means):
            if mean > 0:
                ax.text(bar.get_x() + bar.get_width() / 2., bar.get_height(),
                        valueFmt.format(mean), ha='center', va='bottom',
                        fontsize=7, fontweight='bold')

    ax.set_xlabel('Number of Vehicles', fontsize=11, fontweight='bold')
    ax.set_ylabel(ylabel, fontsize=11, fontweight='bold')
    ax.set_xticks(x)
    ax.set_xticklabels([f'{vc} veh' for vc in VEHICLE_COUNTS])
    ax.legend(bbox_to_anchor=(0.5, -0.15), loc='upper center', ncol=4, fontsize=8)
    ax.grid(True, alpha=0.3, axis='y')
    ax.set_ylim(bottom=0)
    plt.tight_layout(rect=[0, 0.05, 1, 1])

    savePlot(fig, os.path.join(RESULTS_DIR, f'{outputName}_compare.png'))


def plotCompareThroughput(data):
    """Compare-mode throughput bar chart."""
    plotCompareBars(data, 'throughput',
                    'Throughput: Lane Leaders vs All Vehicles\n'
                    'WAVE and UDP across vehicle counts',
                    'Vehicles/second', 'throughput', valueFmt='{:.3f}')


def plotCompareFallbacks(data):
    """Compare-mode fallback rate bar chart."""
    plotCompareBars(data, 'fallbackRate',
                    'Fallback Rate: Lane Leaders vs All Vehicles',
                    'Fallback Rate (%)', 'fallbacks', scale=100, valueFmt='{:.1f}%')


def plotCompareMessages(data):
    """Compare-mode 2-panel message chart (avg per vehicle)."""
    fig, axes = plt.subplots(1, 2, figsize=(14, 5))
    fig.suptitle('Messages per Vehicle: Lane Leaders vs All Vehicles',
                 fontsize=13, fontweight='bold')

    barWidth = 0.18
    x = np.arange(len(VEHICLE_COUNTS))
    configs = [
        ('msgSentPerVehicle', 'Avg Sent per Vehicle'),
        ('msgReceivedPerVehicle', 'Avg Received per Vehicle'),
    ]

    for col, (metric, title) in enumerate(configs):
        ax = axes[col]
        for i, (protocol, mode) in enumerate(COMPARE_SERIES):
            means, stds = getCompareValues(data, protocol, mode, metric)
            offset = (i - 1.5) * barWidth
            bars = ax.bar(x + offset, means, barWidth, yerr=stds,
                          label=COMPARE_LABELS[(protocol, mode)],
                          color=COMPARE_COLORS[(protocol, mode)],
                          hatch=COMPARE_HATCHES[(protocol, mode)],
                          alpha=0.85, capsize=4, edgecolor='black', linewidth=0.5)
            for bar, mean in zip(bars, means):
                if mean > 0:
                    ax.text(bar.get_x() + bar.get_width() / 2., bar.get_height(),
                            f'{mean:.0f}', ha='center', va='bottom',
                            fontsize=6, fontweight='bold')
        ax.set_xlabel('Number of Vehicles', fontsize=10, fontweight='bold')
        ax.set_ylabel('Messages', fontsize=10, fontweight='bold')
        ax.set_title(title, fontsize=11, fontweight='bold')
        ax.set_xticks(x)
        ax.set_xticklabels([f'{vc} veh' for vc in VEHICLE_COUNTS])
        ax.legend(bbox_to_anchor=(0.5, -0.18), loc='upper center', ncol=4, fontsize=7)
        ax.grid(True, alpha=0.3, axis='y')
        ax.set_ylim(bottom=0)

    plt.tight_layout(rect=[0, 0.06, 1, 1])
    savePlot(fig, os.path.join(RESULTS_DIR, 'messages_compare.png'))


def plotCompareCdfWaitTime():
    """Compare-mode CDF of wait time (4 lines per subplot)."""
    fig, axes = plt.subplots(1, len(VEHICLE_COUNTS), figsize=(16, 5), sharey=True)
    fig.suptitle('CDF of Wait Time: Lane Leaders vs All Vehicles\n'
                 'Time from stopping to crossing (RAFT only)',
                 fontsize=13, fontweight='bold')

    linestyles = {
        ('wave', 'laneLeaders'): '-',
        ('wave', 'allVehicles'): '--',
        ('udp', 'laneLeaders'): '-',
        ('udp', 'allVehicles'): '--',
    }

    for col, vc in enumerate(VEHICLE_COUNTS):
        ax = axes[col]
        for protocol in PROTOCOLS:
            for mode in COMPARE_MODES:
                runs = loadRunsForMode(PROTOCOL_PREFIXES[protocol], vc, mode)
                times = []
                for runData in runs:
                    for v in runData:
                        if v.get('coordination_method') == 'fallback':
                            continue
                        wt = v['durations_ms'].get('total_wait_time', 0)
                        if wt > 0:
                            times.append(wt / 1000.0)
                if not times:
                    continue
                sortedT = np.sort(times)
                cdf = np.arange(1, len(sortedT) + 1) / len(sortedT)
                ax.plot(sortedT, cdf,
                        color=COMPARE_COLORS[(protocol, mode)],
                        linestyle=linestyles[(protocol, mode)],
                        linewidth=2,
                        label=f"{COMPARE_LABELS[(protocol, mode)]} (n={len(times)})")

        ax.set_title(f'{vc} Vehicles', fontsize=11, fontweight='bold')
        ax.set_xlabel('Total Wait Time (s)', fontsize=10)
        if col == 0:
            ax.set_ylabel('CDF', fontsize=10)
        ax.set_ylim(0, 1.05)
        ax.set_xlim(left=0)
        ax.legend(fontsize=7, loc='lower right')
        ax.grid(True, alpha=0.3)

    plt.tight_layout()
    savePlot(fig, os.path.join(RESULTS_DIR, 'cdf_compare.png'))


# ============================================================
# Entry Points
# ============================================================

def generateSingleModePlots(mode):
    """Generate all plots for a single cluster mode."""
    print(f"Generating plots for mode: {mode}")
    data = loadDataForMode(mode)
    if not data:
        print(f"  No data found for mode '{mode}'")
        return
    plotThroughput(data, mode)
    plotFallbacks(data, mode)
    plotMessages(data, mode)
    plotCdfWaitTime(mode)
    plotAmbulanceWaitTime(mode)
    print(f"Done — 5 plots saved for {mode}.\n")


def generateModeComparison():
    """Generate side-by-side comparison plots: laneLeaders vs allVehicles."""
    print("Generating comparison plots (laneLeaders vs allVehicles)...")
    data = loadCompareData()
    if not data:
        print("  No data found for comparison")
        return
    plotCompareThroughput(data)
    plotCompareFallbacks(data)
    plotCompareMessages(data)
    plotCompareCdfWaitTime()
    print("Done — 4 comparison plots saved.\n")


def main():
    parser = argparse.ArgumentParser(description='RAFT Intersection Benchmark Plotter')
    parser.add_argument('--simple', action='store_true',
                        help='Use simple_intersection results (required)')
    parser.add_argument('--mode', default='laneLeaders',
                        help='Cluster mode: laneLeaders or allVehicles')
    parser.add_argument('--compare-modes', action='store_true',
                        help='Side-by-side laneLeaders vs allVehicles plots')
    args = parser.parse_args()

    if args.compare_modes:
        generateModeComparison()
        return

    if args.simple:
        generateSingleModePlots(args.mode)
    else:
        print("Use --simple flag. Non-simple mode is deprecated.")


if __name__ == '__main__':
    main()
