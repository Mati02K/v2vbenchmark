#!/usr/bin/env python3
"""
RAFT Channel Utilization Plots

Outputs two files to results/ (top-level, same as throughput_wave.png etc.):
  channel_utilization_heatmap_wave.png    — 3×3 grid: modes × vehicle counts
  channel_utilization_timeseries_wave.png — 1×3 panels: mean utilization per mode per vc

Usage:
  python3 plot_heatmap.py
"""

import os
import glob
import csv
import numpy as np
import matplotlib.pyplot as plt
from matplotlib.lines import Line2D

RESULTS_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'results')
PROTOCOL_PREFIX = 'simple_raftwave'
VEHICLE_COUNTS = [4, 8, 16, 24, 32]

MODES = ['laneLeaders', 'allVehicles', 'allVehicles_multirounds']
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

TIME_GRID_STEP = 0.1
UTILIZATION_CONGESTION_THRESHOLD = 0.30
CMAP = 'YlOrRd'


# ─────────────────────────────────────────────────────────────────────────────
# Data loading
# ─────────────────────────────────────────────────────────────────────────────

def load_utilization_run(run_dir):
    data = {}
    for path in sorted(glob.glob(os.path.join(run_dir, 'channel_V*.csv'))):
        try:
            with open(path, newline='') as f:
                reader = csv.DictReader(f)
                rows = [(float(r['time_s']), float(r['channel_utilization'])) for r in reader]
            if rows:
                vid = int(os.path.basename(path).replace('channel_V', '').replace('.csv', ''))
                data[vid] = rows
        except Exception as e:
            print(f"WARNING: {path}: {e}")
    return data


def build_matrix(utilization_data, time_grid):
    sorted_vids = sorted(utilization_data.keys())
    t_index = {t: i for i, t in enumerate(time_grid)}
    matrix = np.full((len(sorted_vids), len(time_grid)), np.nan)
    for v_idx, vid in enumerate(sorted_vids):
        for t, u in utilization_data[vid]:
            t_key = round(t, 3)
            if t_key in t_index:
                matrix[v_idx, t_index[t_key]] = u
    return sorted_vids, matrix


def load_scenario(mode, vc):
    scenario_dir = os.path.join(RESULTS_DIR, f'{PROTOCOL_PREFIX}_{vc}veh_{mode}')
    run_dirs = sorted(d for d in glob.glob(os.path.join(scenario_dir, 'run_*'))
                      if glob.glob(os.path.join(d, 'channel_V*.csv')))
    if not run_dirs:
        return None, None, None

    all_data, all_times, all_vids = [], set(), set()
    for run_dir in run_dirs:
        data = load_utilization_run(run_dir)
        if data:
            all_data.append(data)
            for vid, rows in data.items():
                all_vids.add(vid)
                for t, _ in rows:
                    all_times.add(round(t, 3))

    if not all_data:
        return None, None, None

    time_grid = sorted(all_times)
    matrices = [build_matrix(d, time_grid)[1] for d in all_data]
    avg_matrix = np.nanmean(np.stack(matrices, axis=0), axis=0)
    return np.array(time_grid), sorted(all_vids), avg_matrix


# ─────────────────────────────────────────────────────────────────────────────
# Plots
# ─────────────────────────────────────────────────────────────────────────────

def plot_heatmap_grid(scenario_data):
    """3-row × 3-col grid of heatmaps. Fixed vmax across all panels."""
    vmax = 0.0
    for mode in MODES:
        for vc in VEHICLE_COUNTS:
            tg, vids, mat = scenario_data.get((mode, vc), (None, None, None))
            if mat is not None and mat.size:
                v = float(np.nanmax(mat))
                if v > vmax:
                    vmax = v
    vmax = max(vmax, UTILIZATION_CONGESTION_THRESHOLD)

    fig, axes = plt.subplots(len(MODES), len(VEHICLE_COUNTS),
                             figsize=(18, 10), squeeze=False)
    fig.subplots_adjust(right=0.88)
    fig.suptitle('Channel Utilization Heatmap — WAVE (802.11p)\n'
                 '10-run average per scenario, fixed colour scale',
                 fontsize=13, fontweight='bold')

    im_ref = None
    for row, mode in enumerate(MODES):
        for col, vc in enumerate(VEHICLE_COUNTS):
            ax = axes[row][col]
            tg, vids, mat = scenario_data.get((mode, vc), (None, None, None))

            if tg is None:
                ax.text(0.5, 0.5, 'No data', ha='center', va='center',
                        transform=ax.transAxes, fontsize=10, color='grey')
                ax.set_xticks([])
                ax.set_yticks([])
            else:
                time_edges = np.append(tg, tg[-1] + TIME_GRID_STEP)
                y_edges = np.arange(len(vids) + 1)
                im = ax.pcolormesh(time_edges, y_edges, mat,
                                   cmap=CMAP, vmin=0.0, vmax=vmax, shading='flat')
                im_ref = im
                ax.set_yticks(np.arange(len(vids)) + 0.5)
                ax.set_yticklabels([f'V{v}' for v in vids], fontsize=6)
                mean_u = float(np.nanmean(mat))
                ax.text(0.99, 0.97, f'mean={mean_u:.3f}',
                        transform=ax.transAxes, ha='right', va='top', fontsize=7,
                        bbox=dict(boxstyle='round,pad=0.2', fc='white', alpha=0.7))

            if row == 0:
                ax.set_title(f'{vc} Vehicles', fontsize=11, fontweight='bold')
            if col == 0:
                ax.set_ylabel(MODE_LABELS[mode], fontsize=10, fontweight='bold')
            if row == len(MODES) - 1:
                ax.set_xlabel('Time (s)', fontsize=9)

    if im_ref is not None:
        cbar_ax = fig.add_axes([0.90, 0.10, 0.02, 0.78])
        cbar = fig.colorbar(im_ref, cax=cbar_ax)
        cbar.set_label('Channel Utilization', fontsize=11)
        cbar.ax.axhline(UTILIZATION_CONGESTION_THRESHOLD, color='black',
                        linewidth=1.2, linestyle='--')
    out = os.path.join(RESULTS_DIR, 'channel_utilization_heatmap_wave.png')
    fig.savefig(out, dpi=150, bbox_inches='tight')
    plt.close(fig)
    print(f"  Saved: channel_utilization_heatmap_wave.png")


def plot_timeseries_grid(scenario_data):
    """1×3 panels (one per vehicle count). Each panel: mean utilization over time per mode."""
    fig, axes = plt.subplots(1, len(VEHICLE_COUNTS), figsize=(16, 5), sharey=True)
    fig.suptitle('Channel Utilization over Time — WAVE (802.11p)\n'
                 'Mean across all vehicles, 10-run average',
                 fontsize=13, fontweight='bold')

    ymax = UTILIZATION_CONGESTION_THRESHOLD * 1.5

    for col, vc in enumerate(VEHICLE_COUNTS):
        ax = axes[col]
        has_data = False

        for mode in MODES:
            tg, vids, mat = scenario_data.get((mode, vc), (None, None, None))
            if tg is None:
                continue
            mean_over_vehicles = np.nanmean(mat, axis=0)
            ax.plot(tg, mean_over_vehicles,
                    color=MODE_COLORS[mode], linewidth=1.5,
                    label=MODE_LABELS[mode])
            peak = float(np.nanmax(mean_over_vehicles))
            if peak > ymax:
                ymax = peak * 1.1
            has_data = True

        ax.axhline(UTILIZATION_CONGESTION_THRESHOLD, color='red', linewidth=1.0,
                   linestyle='--', alpha=0.6)
        ax.set_title(f'{vc} Vehicles', fontsize=11, fontweight='bold')
        ax.set_xlabel('Simulation time (s)', fontsize=10)
        if col == 0:
            ax.set_ylabel('Mean Channel Utilization', fontsize=10)
        ax.grid(True, alpha=0.3)
        ax.set_ylim(0, ymax)

    legend_handles = [Line2D([0], [0], color=MODE_COLORS[m], linewidth=2,
                              label=MODE_LABELS[m]) for m in MODES]
    legend_handles.append(Line2D([0], [0], color='red', linewidth=1,
                                  linestyle='--', alpha=0.6,
                                  label=f'Threshold ({UTILIZATION_CONGESTION_THRESHOLD:.0%})'))
    fig.legend(handles=legend_handles,
               bbox_to_anchor=(1.01, 0.5), loc='center left', fontsize=9)

    plt.tight_layout()
    out = os.path.join(RESULTS_DIR, 'channel_utilization_timeseries_wave.png')
    fig.savefig(out, dpi=150, bbox_inches='tight')
    plt.close(fig)
    print(f"  Saved: channel_utilization_timeseries_wave.png")


# ─────────────────────────────────────────────────────────────────────────────
# Entry point
# ─────────────────────────────────────────────────────────────────────────────

def main():
    print("Loading channel utilization data...")
    scenario_data = {}
    for mode in MODES:
        for vc in VEHICLE_COUNTS:
            tg, vids, mat = load_scenario(mode, vc)
            if tg is not None:
                scenario_data[(mode, vc)] = (tg, vids, mat)
                print(f"  Loaded: {mode} {vc}veh")

    if not scenario_data:
        print("No channel utilization data found. Run a WAVE benchmark first.")
        return

    print("Generating plots...")
    plot_heatmap_grid(scenario_data)
    plot_timeseries_grid(scenario_data)
    print(f"\nDone — 2 plots saved to {RESULTS_DIR}/")


if __name__ == '__main__':
    main()
