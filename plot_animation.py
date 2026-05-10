#!/usr/bin/env python3
"""
Channel utilization progression animations: 4 → 8 → 16 vehicles.

Generates two GIFs, one per mode, using only priority-rotation result dirs:
  results/channel_utilization_animation_laneleaders.gif
  results/channel_utilization_animation_allvehicles.gif

Usage:
  python3 plot_animation.py
"""

import os
import sys
import glob
import csv
import numpy as np
import matplotlib.pyplot as plt
import matplotlib.animation as animation

RESULTS_DIR     = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'results')
VEHICLE_COUNTS  = [4, 8, 16, 24, 32]
MAX_VEHICLES    = 32
PROTOCOL_PREFIX = 'simple_raftwave'

TIME_GRID_STEP = 0.1
CMAP           = 'YlOrRd'
HOLD_FRAMES    = 8
FPS            = 4

MODES = [
    ('laneLeaders', 'laneLeaders',  'channel_utilization_animation_laneleaders.gif'),
    ('allVehicles', 'allVehicles',  'channel_utilization_animation_allvehicles.gif'),
]


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
            print(f"WARNING: could not read {path}: {e}")
    return data


def build_padded_matrix(utilization_data, time_grid):
    t_index = {round(t, 3): i for i, t in enumerate(time_grid)}
    matrix = np.full((MAX_VEHICLES, len(time_grid)), np.nan)
    for vid, rows in utilization_data.items():
        if vid >= MAX_VEHICLES:
            continue
        for t, u in rows:
            t_key = round(t, 3)
            if t_key in t_index:
                matrix[vid, t_index[t_key]] = u
    return matrix


def load_scenario(dir_suffix, vc, time_grid_ref):
    scenario_dir = os.path.join(RESULTS_DIR, f'{PROTOCOL_PREFIX}_{vc}veh_{dir_suffix}')
    run_dirs = sorted(d for d in glob.glob(os.path.join(scenario_dir, 'run_*'))
                      if glob.glob(os.path.join(d, 'channel_V*.csv')))
    if not run_dirs:
        return None

    all_times = set()
    all_data = []
    for run_dir in run_dirs:
        data = load_utilization_run(run_dir)
        if data:
            all_data.append(data)
            for rows in data.values():
                for t, _ in rows:
                    all_times.add(round(t, 3))

    if not all_data:
        return None

    time_grid = sorted(all_times) if time_grid_ref is None else time_grid_ref
    matrices = [build_padded_matrix(d, time_grid) for d in all_data]
    return np.array(time_grid), np.nanmean(np.stack(matrices, axis=0), axis=0)


def generate_gif(mode_label, dir_suffix, out_filename):
    all_times = set()
    raw = {}
    for vc in VEHICLE_COUNTS:
        result = load_scenario(dir_suffix, vc, None)
        if result is None:
            print(f"  [{mode_label}] No data for {vc}veh — skipping.")
            continue
        tg, mat = result
        raw[vc] = (tg, mat)
        for t in tg:
            all_times.add(round(t, 3))

    if not raw:
        print(f"  [{mode_label}] No data found — skipping GIF.")
        return

    time_grid = np.array(sorted(all_times))

    scenarios = {}
    for vc in raw:
        result = load_scenario(dir_suffix, vc, list(time_grid))
        if result:
            scenarios[vc] = result[1]

    vmax = max(float(np.nanmax(m)) for m in scenarios.values())
    vmax = max(vmax, 0.05)

    time_edges = np.append(time_grid, time_grid[-1] + TIME_GRID_STEP)
    y_edges = np.arange(MAX_VEHICLES + 1)

    frames = []
    for vc in VEHICLE_COUNTS:
        if vc in scenarios:
            for _ in range(HOLD_FRAMES):
                frames.append((vc, scenarios[vc]))

    fig, ax = plt.subplots(figsize=(14, 5))
    fig.patch.set_facecolor('#1a1a2e')
    ax.set_facecolor('#1a1a2e')

    dummy = np.full((MAX_VEHICLES, len(time_grid)), np.nan)
    mesh = ax.pcolormesh(time_edges, y_edges, dummy,
                         cmap=CMAP, vmin=0.0, vmax=vmax, shading='flat')

    cbar = fig.colorbar(mesh, ax=ax, pad=0.02)
    cbar.set_label('Channel Utilization', fontsize=11, color='white')
    cbar.ax.yaxis.set_tick_params(color='white')
    plt.setp(cbar.ax.yaxis.get_ticklabels(), color='white')

    ax.set_xlabel('Simulation time (s)', fontsize=12, color='white')
    ax.set_ylabel('Vehicle ID', fontsize=12, color='white')
    ax.set_yticks(np.arange(MAX_VEHICLES) + 0.5)
    ax.set_yticklabels([f'V{v}' for v in range(MAX_VEHICLES)], fontsize=7, color='white')
    ax.tick_params(colors='white')
    for spine in ax.spines.values():
        spine.set_edgecolor('white')

    title = ax.set_title('', fontsize=13, fontweight='bold', color='white')
    stats_text = ax.text(0.99, 0.97, '', transform=ax.transAxes,
                         ha='right', va='top', fontsize=9, color='white',
                         bbox=dict(boxstyle='round,pad=0.3', fc='#1a1a2e', alpha=0.8))

    def update(frame_idx):
        vc, matrix = frames[frame_idx]
        mesh.set_array(matrix.ravel())
        title.set_text(f'Channel Utilization — WAVE / {mode_label} — {vc} vehicles')
        active = matrix[~np.isnan(matrix)]
        mean_u = float(np.nanmean(active)) if active.size else 0.0
        peak_u = float(np.nanmax(active)) if active.size else 0.0
        stats_text.set_text(f'mean={mean_u:.4f}  peak={peak_u:.4f}')
        return mesh, title, stats_text

    anim = animation.FuncAnimation(fig, update, frames=len(frames),
                                   interval=1000 // FPS, blit=False)

    out_path = os.path.join(RESULTS_DIR, out_filename)
    anim.save(out_path, writer=animation.PillowWriter(fps=FPS))
    plt.close(fig)
    print(f"  Saved: {out_filename}")


def main():
    os.makedirs(RESULTS_DIR, exist_ok=True)
    for mode_label, dir_suffix, out_filename in MODES:
        print(f"Generating {out_filename}...")
        generate_gif(mode_label, dir_suffix, out_filename)


if __name__ == '__main__':
    main()
