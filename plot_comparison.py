#!/usr/bin/env python3
"""
RAFT Intersection Benchmark Plotter
Generates 13 comparison plots: laneLeaders vs allVehicles vs multirounds,
priority vs nopriority, across UDP and WAVE transports.

Usage:
  python3 plot_comparison.py          # generate all 13 comparison plots
"""

import json
import os
import numpy as np
import matplotlib.pyplot as plt
from matplotlib.lines import Line2D
from matplotlib.patches import Patch

RESULTS_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'results')
VEHICLE_COUNTS = [4, 8, 16]
PROTOCOLS = ['wave', 'udp']
PROTOCOL_PREFIXES = {'wave': 'simple_raftwave', 'udp': 'simple_udp'}
PROTOCOL_LABELS = {'wave': 'WAVE (802.11p)', 'udp': 'UDP (802.11a)'}

MODES = ['laneLeaders', 'allVehicles', 'allVehicles_multirounds']
STANDARD_MODES = ['laneLeaders', 'allVehicles']
MODE_LABELS = {
    'laneLeaders':             'Lane Leaders',
    'allVehicles':             'All Vehicles',
    'allVehicles_multirounds': 'Multi-Rounds',
}
MODE_COLORS = {
    'laneLeaders':             '#3498db',
    'allVehicles':             '#2ecc71',
    'allVehicles_multirounds': '#e67e22',
}

PRIORITY_VARIANTS = ['priority', 'nopriority']
MODE_DIRS = {
    'laneLeaders':             {'priority': 'laneLeaders',             'nopriority': 'laneLeaders_nopriority'},
    'allVehicles':             {'priority': 'allVehicles',             'nopriority': 'allVehicles_nopriority'},
    'allVehicles_multirounds': {'priority': 'allVehicles_multirounds', 'nopriority': 'allVehicles_nopriority_multirounds'},
}
PRIORITY_LINESTYLES = {'priority': '-', 'nopriority': '--'}
PRIORITY_DISPLAY = {'priority': 'Priority', 'nopriority': 'No Priority'}

PRIO_COLOR = '#e74c3c'
NORM_COLOR = '#95a5a6'


# ============================================================
# Data Loading
# ============================================================

def loadRunsForMode(protocolPrefix, vc, mode):
    """Loads run JSONs. Tolerant of truncated files (missing closing bracket)
    that the simulator can produce if it gets cut off mid-write."""
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
    metrics = {
        'leaderElectionTime':  [],
        'decisionLatency':     [],
        'throughput':          [],
        'waitTime':            [],
        'msgSentPerVehicle':   [],
        'msgRecvPerVehicle':   [],
        'fallbackRate':        [],
    }

    for runData in runsData:
        if not runData:
            continue
        numVehicles = len(runData)
        raftVehicles = [v for v in runData if v.get('coordination_method') != 'fallback']

        electionPerVeh = [v['durations_ms'].get('leader_election_time_ms', 0)
                          for v in raftVehicles
                          if v['durations_ms'].get('leader_election_time_ms', 0) > 0]
        if electionPerVeh:
            metrics['leaderElectionTime'].append(np.mean(electionPerVeh))

        decisionPerVeh = [v['durations_ms'].get('decision_latency_ms', 0)
                          for v in raftVehicles
                          if v['durations_ms'].get('decision_latency_ms', 0) > 0]
        if decisionPerVeh:
            metrics['decisionLatency'].append(np.mean(decisionPerVeh))

        waitTimes = [
            v['timestamps_ms']['passed'] - v['timestamps_ms']['stopped']
            for v in raftVehicles
            if v['timestamps_ms'].get('passed', 0) > 0 and v['timestamps_ms'].get('stopped', 0) > 0
        ]
        if waitTimes:
            avgWaitMs = sum(waitTimes) / len(waitTimes)
            if avgWaitMs > 0:
                metrics['throughput'].append(len(raftVehicles) / (avgWaitMs / 1000.0))

        fallbackCount = sum(1 for v in runData if v.get('coordination_method') == 'fallback')
        metrics['fallbackRate'].append(fallbackCount / numVehicles if numVehicles > 0 else 0)

        totalSent = sum(v['messages']['sent'] for v in runData)
        totalRecv = sum(v['messages']['received'] for v in runData)
        metrics['msgSentPerVehicle'].append(totalSent / numVehicles if numVehicles > 0 else 0)
        metrics['msgRecvPerVehicle'].append(totalRecv / numVehicles if numVehicles > 0 else 0)

        for v in runData:
            metrics['waitTime'].append(v['durations_ms']['total_wait_time'])

    result = {}
    for key, vals in metrics.items():
        if vals:
            result[key] = {'mean': float(np.mean(vals)), 'std': float(np.std(vals))}
        else:
            result[key] = {'mean': 0, 'std': 0}
    return result


def loadAllData():
    # Missing result directories are silently skipped — partial benchmarks should still plot.
    data = {}
    for protocol in PROTOCOLS:
        prefix = PROTOCOL_PREFIXES[protocol]
        for mode in MODES:
            for prio in PRIORITY_VARIANTS:
                fullMode = MODE_DIRS[mode][prio]
                for vc in VEHICLE_COUNTS:
                    runs = loadRunsForMode(prefix, vc, fullMode)
                    if runs:
                        data[(protocol, mode, prio, vc)] = calculateMetrics(runs)
    return data


# ============================================================
# Plot Helpers
# ============================================================

def savePlot(fig, path):
    fig.savefig(path, dpi=150, bbox_inches='tight')
    plt.close(fig)
    print(f"  Saved: {os.path.basename(path)}")


def getValues(data, protocol, mode, prio, metric, scale=1.0):
    means, stds = [], []
    for vc in VEHICLE_COUNTS:
        key = (protocol, mode, prio, vc)
        if key in data and metric in data[key]:
            means.append(data[key][metric]['mean'] * scale)
            stds.append(data[key][metric]['std'] * scale)
        else:
            means.append(None)
            stds.append(None)
    return means, stds


def buildLegend(modes=None):
    if modes is None:
        modes = MODES
    handles = []
    for mode in modes:
        handles.append(Line2D([0], [0], color=MODE_COLORS[mode], linewidth=2,
                               label=MODE_LABELS[mode]))
    for prio in PRIORITY_VARIANTS:
        handles.append(Line2D([0], [0], color='black',
                               linestyle=PRIORITY_LINESTYLES[prio], linewidth=1.5,
                               label=PRIORITY_DISPLAY[prio]))
    return handles


def buildModeLegend(modes=None):
    if modes is None:
        modes = MODES
    return [Line2D([0], [0], color=MODE_COLORS[mode], linewidth=2, label=MODE_LABELS[mode])
            for mode in modes]


# ============================================================
# Non-Ambulance Line Plots (per transport)
# ============================================================

def plotMetricLine(data, transport, metric, title, ylabel, outputPrefix, scale=1.0, modes=None):
    if modes is None:
        modes = MODES
    fig, ax = plt.subplots(figsize=(8, 5))
    fig.suptitle(f'{title} — {PROTOCOL_LABELS[transport]}',
                 fontsize=13, fontweight='bold')

    x = np.array(VEHICLE_COUNTS)
    hasAny = False

    for mode in modes:
        for prio in PRIORITY_VARIANTS:
            means, _ = getValues(data, transport, mode, prio, metric, scale)
            if all(v is None for v in means):
                continue
            yVals = [v if v is not None else np.nan for v in means]
            ax.plot(x, yVals,
                    color=MODE_COLORS[mode],
                    linestyle=PRIORITY_LINESTYLES[prio],
                    linewidth=2, marker='o', markersize=6,
                    label=f"{MODE_LABELS[mode]} / {PRIORITY_DISPLAY[prio]}")
            hasAny = True

    if not hasAny:
        plt.close(fig)
        return

    ax.set_xlabel('Number of Vehicles', fontsize=11, fontweight='bold')
    ax.set_ylabel(ylabel, fontsize=11, fontweight='bold')
    ax.set_xticks(VEHICLE_COUNTS)
    ax.set_xticklabels([f'{vc} veh' for vc in VEHICLE_COUNTS])
    ax.legend(handles=buildLegend(modes),
              bbox_to_anchor=(1.01, 1), loc='upper left', fontsize=8, borderaxespad=0)
    ax.grid(True, alpha=0.3)
    ax.set_ylim(bottom=0)
    plt.tight_layout()
    savePlot(fig, os.path.join(RESULTS_DIR, f'{outputPrefix}_{transport}.png'))


def plotThroughput(data, transport):
    plotMetricLine(data, transport, 'throughput',
                   'System Throughput', 'Vehicles / second', 'throughput')


def plotLeaderElectionTime(data, transport):
    plotMetricLine(data, transport, 'leaderElectionTime',
                   'RAFT Leader Election Time (cluster formed → leader elected)',
                   'Leader Election Time (ms)', 'leader_election_time',
                   modes=STANDARD_MODES)


def plotDecisionTime(data, transport):
    plotMetricLine(data, transport, 'decisionLatency',
                   'RAFT Decision Latency (status collection → schedule committed)',
                   'Decision Latency (ms)', 'decision_time',
                   modes=STANDARD_MODES)


def plotFallbacks(data, transport):
    plotMetricLine(data, transport, 'fallbackRate',
                   'Fallback Rate', 'Fallback Rate (%)', 'fallbacks', scale=100)


def plotMessages(data, transport):
    fig, axes = plt.subplots(1, 2, figsize=(14, 5))
    fig.suptitle(f'Messages per Vehicle — {PROTOCOL_LABELS[transport]}',
                 fontsize=13, fontweight='bold')

    configs = [
        ('msgSentPerVehicle',  'Avg Sent per Vehicle'),
        ('msgRecvPerVehicle',  'Avg Received per Vehicle'),
    ]
    x = np.array(VEHICLE_COUNTS)
    hasAny = False

    for col, (metric, panelTitle) in enumerate(configs):
        ax = axes[col]
        for mode in MODES:
            allMeans = []
            for prio in PRIORITY_VARIANTS:
                means, _ = getValues(data, transport, mode, prio, metric)
                if not all(v is None for v in means):
                    allMeans.append(means)
            if not allMeans:
                continue
            yVals = []
            for i in range(len(VEHICLE_COUNTS)):
                vals = [m[i] for m in allMeans if m[i] is not None]
                yVals.append(float(np.mean(vals)) if vals else np.nan)
            ax.plot(x, yVals,
                    color=MODE_COLORS[mode],
                    linewidth=2, marker='o', markersize=6)
            hasAny = True
        ax.set_xlabel('Number of Vehicles', fontsize=10, fontweight='bold')
        ax.set_ylabel('Messages', fontsize=10, fontweight='bold')
        ax.set_title(panelTitle, fontsize=11, fontweight='bold')
        ax.set_xticks(VEHICLE_COUNTS)
        ax.set_xticklabels([f'{vc} veh' for vc in VEHICLE_COUNTS])
        ax.grid(True, alpha=0.3)
        ax.set_ylim(bottom=0)

    if not hasAny:
        plt.close(fig)
        return

    fig.legend(handles=buildModeLegend(),
               bbox_to_anchor=(1.01, 0.5), loc='center left', fontsize=8, borderaxespad=0)
    plt.tight_layout()
    savePlot(fig, os.path.join(RESULTS_DIR, f'messages_{transport}.png'))


def plotCdf(transport):
    fig, axes = plt.subplots(1, len(VEHICLE_COUNTS), figsize=(15, 5), sharey=True)
    fig.suptitle(f'CDF of Total Wait Time — {PROTOCOL_LABELS[transport]}\n'
                 f'Time from stopping to crossing (RAFT vehicles only)',
                 fontsize=13, fontweight='bold')

    for col, vc in enumerate(VEHICLE_COUNTS):
        ax = axes[col]
        for mode in MODES:
            for prio in PRIORITY_VARIANTS:
                fullMode = MODE_DIRS[mode][prio]
                runs = loadRunsForMode(PROTOCOL_PREFIXES[transport], vc, fullMode)
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
                        color=MODE_COLORS[mode],
                        linestyle=PRIORITY_LINESTYLES[prio],
                        linewidth=2)

        ax.set_title(f'{vc} Vehicles', fontsize=11, fontweight='bold')
        ax.set_xlabel('Wait Time (s)', fontsize=10)
        if col == 0:
            ax.set_ylabel('CDF', fontsize=10)
        ax.set_ylim(0, 1.05)
        ax.set_xlim(left=0)
        ax.grid(True, alpha=0.3)

    fig.legend(handles=buildLegend(),
               bbox_to_anchor=(1.01, 0.5), loc='center left', fontsize=8, borderaxespad=0)
    plt.tight_layout()
    savePlot(fig, os.path.join(RESULTS_DIR, f'cdf_{transport}.png'))


# ============================================================
# Ambulance Graph (priority vs nopriority across all combos)
# ============================================================

def plotPriority():
    # Multirounds is excluded: too many fallbacks make the priority-vs-normal comparison meaningless.
    fig, axes = plt.subplots(1, len(VEHICLE_COUNTS), figsize=(18, 6), sharey=False)
    fig.suptitle('Priority Vehicle Benefit\n'
                 'Priority vehicle crossing time vs normal vs no-priority run',
                 fontsize=13, fontweight='bold')

    priorityModes = [m for m in MODES if m != 'allVehicles_multirounds']
    barW = 0.2
    combos = [(t, m) for t in ['wave'] for m in priorityModes]
    comboLabels = [f"WAVE\n{MODE_LABELS[m][:4]}" for t, m in combos]
    x = np.arange(len(combos))

    for col, vc in enumerate(VEHICLE_COUNTS):
        ax = axes[col]
        prioVals, normVals, noPrioVals = [], [], []

        for transport, mode in combos:
            prefix = PROTOCOL_PREFIXES[transport]

            prioRuns = loadRunsForMode(prefix, vc, MODE_DIRS[mode]['priority'])
            noPrioRuns = loadRunsForMode(prefix, vc, MODE_DIRS[mode]['nopriority'])

            prioTimes, normTimes = [], []
            for runData in prioRuns:
                for v in runData:
                    if v.get('coordination_method') == 'fallback':
                        continue
                    passed = v['timestamps_ms'].get('passed', 0)
                    stopped = v['timestamps_ms'].get('stopped', 0)
                    if passed <= 0 or stopped <= 0:
                        continue
                    latency = (passed - stopped) / 1000.0
                    if v.get('is_priority_vehicle', False):
                        prioTimes.append(latency)
                    else:
                        normTimes.append(latency)

            noPrioTimes = []
            for runData in noPrioRuns:
                for v in runData:
                    if v.get('coordination_method') == 'fallback':
                        continue
                    passed = v['timestamps_ms'].get('passed', 0)
                    stopped = v['timestamps_ms'].get('stopped', 0)
                    if passed <= 0 or stopped <= 0:
                        continue
                    noPrioTimes.append((passed - stopped) / 1000.0)

            prioVals.append(np.mean(prioTimes) if prioTimes else 0)
            normVals.append(np.mean(normTimes) if normTimes else 0)
            noPrioVals.append(np.mean(noPrioTimes) if noPrioTimes else 0)

        ax.bar(x - barW, prioVals,  barW, label='Priority veh (prio run)',
               color=PRIO_COLOR, alpha=0.85, edgecolor='black', linewidth=0.5)
        ax.bar(x,         normVals,  barW, label='Normal veh (prio run)',
               color=NORM_COLOR, alpha=0.85, edgecolor='black', linewidth=0.5)
        ax.bar(x + barW,  noPrioVals, barW, label='All veh (noprio run)',
               color='#7f8c8d', alpha=0.85, edgecolor='black', linewidth=0.5,
               hatch='//')

        ax.set_title(f'{vc} Vehicles', fontsize=11, fontweight='bold')
        ax.set_xticks(x)
        ax.set_xticklabels(comboLabels, fontsize=7)
        if col == 0:
            ax.set_ylabel('Mean Crossing Time (s)', fontsize=10)
        ax.grid(True, alpha=0.3, axis='y')
        ax.set_ylim(bottom=0)

    legendHandles = [
        Patch(facecolor=PRIO_COLOR, edgecolor='black', label='Priority vehicle (priority run)'),
        Patch(facecolor=NORM_COLOR, edgecolor='black', label='Normal vehicles (priority run)'),
        Patch(facecolor='#7f8c8d', edgecolor='black', hatch='//', label='All vehicles (nopriority run)'),
    ]
    fig.legend(handles=legendHandles,
               bbox_to_anchor=(0.5, -0.02), loc='upper center', ncol=3, fontsize=9)
    plt.tight_layout()
    savePlot(fig, os.path.join(RESULTS_DIR, 'priority.png'))


# ============================================================
# Entry Point
# ============================================================

def generateAllPlots():
    print("Loading data for all modes and priority variants...")
    data = loadAllData()

    if not data:
        print("No result data found. Run the benchmark first.")
        return

    print("Generating plots...")
    for transport in PROTOCOLS:
        plotThroughput(data, transport)
        plotLeaderElectionTime(data, transport)
        plotDecisionTime(data, transport)
        plotFallbacks(data, transport)
        plotMessages(data, transport)
        plotCdf(transport)

    plotPriority()
    print(f"\nDone — 13 plots saved to {RESULTS_DIR}/")


if __name__ == '__main__':
    generateAllPlots()
