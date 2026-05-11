#!/usr/bin/env python3
"""
RAFT SINR Plots

Outputs to results/ (same directory as other plots):
  sinr_heatmap_wave.png          — mean SINR (dB) per vehicle per 100 ms, modes × vehicle counts grid
  sinr_timeseries_wave.png       — mean SINR per mode per vehicle count (time series)
  sinr_animation_cluster.gif     — animated SINR heatmap across vehicle counts (cluster mode)
  sinr_animation_allvehicles.gif — animated SINR heatmap across vehicle counts (allVehicles mode)

Usage:
  python3 plot_sinr.py
"""

import os
import glob
import csv
import math
import numpy as np
import matplotlib.pyplot as plt
import matplotlib.animation as animation
from matplotlib.lines import Line2D

RESULTS_DIR     = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'results')
PROTOCOL_PREFIX = 'simple_raftwave'
VEHICLE_COUNTS  = [4, 8, 16, 20]
MAX_VEHICLES    = 20

MODES = ['cluster', 'allVehicles']
MODE_LABELS = {
    'cluster':                 'Cluster',
    'allVehicles':             'All Vehicles',
    'allVehicles_multirounds': 'Multi-Rounds',
}
MODE_COLORS = {
    'cluster':                 '#3498db',
    'allVehicles':             '#2ecc71',
    'allVehicles_multirounds': '#e67e22',
}

TIME_GRID_STEP = 0.1
CMAP           = 'plasma'
HOLD_FRAMES    = 8
FPS            = 4


# ── Data loading ──────────────────────────────────────────────────────────────

def load_sinr_run(run_dir):
    """Return {vehicle_id: [(time_s, sinr_db_mean), ...]} for one run directory."""
    data = {}
    for path in sorted(glob.glob(os.path.join(run_dir, 'sinr_V*.csv'))):
        try:
            with open(path, newline='') as f:
                reader = csv.DictReader(f)
                rows = []
                for r in reader:
                    try:
                        t = float(r['time_s'])
                        v = float(r['sinr_db_mean'])
                        rows.append((t, v))
                    except (ValueError, KeyError):
                        pass
            if rows:
                vid = int(os.path.basename(path).replace('sinr_V', '').replace('.csv', ''))
                data[vid] = rows
        except Exception as e:
            print(f"WARNING: could not read {path}: {e}")
    return data


def build_padded_matrix(sinr_data, time_grid):
    """Map per-vehicle SINR samples onto the common time grid (NaN for empty windows)."""
    t_index = {round(t, 3): i for i, t in enumerate(time_grid)}
    matrix = np.full((MAX_VEHICLES, len(time_grid)), np.nan)
    for vid, rows in sinr_data.items():
        if vid >= MAX_VEHICLES:
            continue
        for t, v in rows:
            t_key = round(t, 3)
            if t_key in t_index:
                matrix[vid, t_index[t_key]] = v
    return matrix


def load_scenario(dir_suffix, vc, time_grid_ref=None):
    """Load and average all runs for one (mode, vc) scenario.
    Returns (time_grid_array, mean_matrix) or None if no data."""
    scenario_dir = os.path.join(RESULTS_DIR, f'{PROTOCOL_PREFIX}_{vc}veh_{dir_suffix}')
    run_dirs = sorted(d for d in glob.glob(os.path.join(scenario_dir, 'run_*'))
                      if glob.glob(os.path.join(d, 'sinr_V*.csv')))
    if not run_dirs:
        return None

    all_times = set()
    all_data = []
    for run_dir in run_dirs:
        d = load_sinr_run(run_dir)
        if d:
            all_data.append(d)
            for rows in d.values():
                for t, _ in rows:
                    all_times.add(round(t, 3))

    if not all_data:
        return None

    time_grid = sorted(all_times) if time_grid_ref is None else time_grid_ref
    matrices = [build_padded_matrix(d, time_grid) for d in all_data]
    mean_mat = np.nanmean(np.stack(matrices, axis=0), axis=0)
    return np.array(time_grid), mean_mat


# ── Heatmap ───────────────────────────────────────────────────────────────────

def plot_heatmap():
    fig, axes = plt.subplots(len(MODES), len(VEHICLE_COUNTS),
                             figsize=(5 * len(VEHICLE_COUNTS), 4 * len(MODES)),
                             squeeze=False)
    fig.patch.set_facecolor('#1a1a2e')

    vmin, vmax = math.inf, -math.inf
    data_cache = {}
    for mode in MODES:
        for vc in VEHICLE_COUNTS:
            result = load_scenario(mode, vc)
            if result:
                data_cache[(mode, vc)] = result
                valid = result[1][~np.isnan(result[1])]
                if valid.size:
                    vmin = min(vmin, float(valid.min()))
                    vmax = max(vmax, float(valid.max()))

    if math.isinf(vmin):
        print("  [heatmap] No SINR data found — skipping heatmap.")
        plt.close(fig)
        return

    for row, mode in enumerate(MODES):
        for col, vc in enumerate(VEHICLE_COUNTS):
            ax = axes[row][col]
            ax.set_facecolor('#1a1a2e')
            if (mode, vc) in data_cache:
                tg, mat = data_cache[(mode, vc)]
                te = np.append(tg, tg[-1] + TIME_GRID_STEP)
                ye = np.arange(MAX_VEHICLES + 1)
                im = ax.pcolormesh(te, ye, mat, cmap=CMAP,
                                   vmin=vmin, vmax=vmax, shading='flat')
                if col == len(VEHICLE_COUNTS) - 1:
                    cb = fig.colorbar(im, ax=ax, pad=0.02)
                    cb.set_label('SINR (dB)', fontsize=8, color='white')
                    cb.ax.yaxis.set_tick_params(color='white')
                    plt.setp(cb.ax.yaxis.get_ticklabels(), color='white')
            else:
                ax.text(0.5, 0.5, 'No data', transform=ax.transAxes,
                        ha='center', va='center', color='white', fontsize=9)

            if row == 0:
                ax.set_title(f'{vc} vehicles', fontsize=10, color='white', fontweight='bold')
            if col == 0:
                ax.set_ylabel(MODE_LABELS.get(mode, mode), fontsize=9, color='white')
            ax.tick_params(colors='white', labelsize=7)
            ax.set_xlabel('Time (s)' if row == len(MODES) - 1 else '', fontsize=8, color='white')
            for sp in ax.spines.values():
                sp.set_edgecolor('#444')

    fig.suptitle('Per-Vehicle SINR (dB) — WAVE', fontsize=14, color='white', fontweight='bold', y=1.01)
    plt.tight_layout()
    out = os.path.join(RESULTS_DIR, 'sinr_heatmap_wave.png')
    plt.savefig(out, dpi=120, bbox_inches='tight', facecolor=fig.get_facecolor())
    plt.close(fig)
    print(f"  Saved: sinr_heatmap_wave.png")


# ── Time series ───────────────────────────────────────────────────────────────

def plot_timeseries():
    fig, axes = plt.subplots(1, len(VEHICLE_COUNTS),
                             figsize=(5 * len(VEHICLE_COUNTS), 4), sharey=True)
    fig.patch.set_facecolor('#1a1a2e')
    if len(VEHICLE_COUNTS) == 1:
        axes = [axes]

    has_any = False
    for col, vc in enumerate(axes):
        ax = axes[col]
        ax.set_facecolor('#1a1a2e')
        ax.tick_params(colors='white', labelsize=8)
        for sp in ax.spines.values():
            sp.set_edgecolor('#444')
        ax.set_title(f'{VEHICLE_COUNTS[col]} vehicles', fontsize=10,
                     color='white', fontweight='bold')
        ax.set_xlabel('Time (s)', fontsize=9, color='white')
        if col == 0:
            ax.set_ylabel('Mean SINR (dB)', fontsize=9, color='white')

        for mode in MODES:
            result = load_scenario(mode, VEHICLE_COUNTS[col])
            if result is None:
                continue
            tg, mat = result
            mean_over_vehicles = np.nanmean(mat, axis=0)
            if np.all(np.isnan(mean_over_vehicles)):
                continue
            ax.plot(tg, mean_over_vehicles,
                    color=MODE_COLORS.get(mode, 'white'),
                    label=MODE_LABELS.get(mode, mode),
                    linewidth=1.4, alpha=0.85)
            has_any = True

    if not has_any:
        print("  [timeseries] No SINR data found — skipping timeseries.")
        plt.close(fig)
        return

    legend_handles = [Line2D([0], [0], color=MODE_COLORS[m], linewidth=2,
                              label=MODE_LABELS[m])
                      for m in MODES if MODE_COLORS.get(m)]
    fig.legend(handles=legend_handles, loc='upper right',
               facecolor='#1a1a2e', edgecolor='white', labelcolor='white', fontsize=9)
    fig.suptitle('Fleet-Mean SINR Over Time — WAVE', fontsize=13,
                 color='white', fontweight='bold')
    plt.tight_layout()
    out = os.path.join(RESULTS_DIR, 'sinr_timeseries_wave.png')
    plt.savefig(out, dpi=120, bbox_inches='tight', facecolor=fig.get_facecolor())
    plt.close(fig)
    print(f"  Saved: sinr_timeseries_wave.png")


# ── Animation ─────────────────────────────────────────────────────────────────

def generate_gif(mode_label, dir_suffix, out_filename, shared_vmin, shared_vmax):
    all_times = set()
    raw = {}
    for vc in VEHICLE_COUNTS:
        result = load_scenario(dir_suffix, vc, None)
        if result is None:
            print(f"  [{mode_label}] No SINR data for {vc}veh — skipping.")
            continue
        tg, mat = result
        raw[vc] = (tg, mat)
        for t in tg:
            all_times.add(round(t, 3))

    if not raw:
        print(f"  [{mode_label}] No SINR data found — skipping GIF.")
        return

    time_grid = np.array(sorted(all_times))

    scenarios = {}
    for vc in raw:
        result = load_scenario(dir_suffix, vc, list(time_grid))
        if result:
            scenarios[vc] = result[1]

    # Use shared scale so both GIFs are directly comparable.
    vmin = shared_vmin
    vmax = shared_vmax

    time_edges = np.append(time_grid, time_grid[-1] + TIME_GRID_STEP)
    y_edges    = np.arange(MAX_VEHICLES + 1)

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
                         cmap=CMAP, vmin=vmin, vmax=vmax, shading='flat')

    cbar = fig.colorbar(mesh, ax=ax, pad=0.02)
    cbar.set_label('SINR (dB)', fontsize=11, color='white')
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

    def update(frame_idx):
        vc, matrix = frames[frame_idx]
        mesh.set_array(matrix.ravel())
        title.set_text(f'SINR (dB) — WAVE / {mode_label} — {vc} vehicles')
        return mesh, title

    anim = animation.FuncAnimation(fig, update, frames=len(frames),
                                   interval=1000 // FPS, blit=False)

    out_path = os.path.join(RESULTS_DIR, out_filename)
    anim.save(out_path, writer=animation.PillowWriter(fps=FPS))
    plt.close(fig)
    print(f"  Saved: {out_filename}")


# ── Entry point ───────────────────────────────────────────────────────────────

def main():
    os.makedirs(RESULTS_DIR, exist_ok=True)

    print("Generating sinr_heatmap_wave.png...")
    plot_heatmap()

    print("Generating sinr_timeseries_wave.png...")
    plot_timeseries()

    ANIM_MODES = [
        ('Cluster',     'cluster',     'sinr_animation_cluster.gif'),
        ('All Vehicles','allVehicles', 'sinr_animation_allvehicles.gif'),
    ]

    # Compute shared vmin/vmax across ALL modes so both SINR GIFs use the same scale.
    all_vals = []
    for _, suffix, _ in ANIM_MODES:
        for vc in VEHICLE_COUNTS:
            result = load_scenario(suffix, vc, None)
            if result:
                valid = result[1][~np.isnan(result[1])]
                if valid.size:
                    all_vals.append(valid)
    if all_vals:
        combined = np.concatenate(all_vals)
        shared_vmin = float(np.nanpercentile(combined, 2))
        shared_vmax = float(np.nanpercentile(combined, 98))
    else:
        shared_vmin, shared_vmax = -10.0, 30.0
    print(f"  Shared SINR scale for GIFs: vmin={shared_vmin:.1f} dB  vmax={shared_vmax:.1f} dB")

    for label, suffix, fname in ANIM_MODES:
        print(f"Generating {fname}...")
        generate_gif(label, suffix, fname, shared_vmin, shared_vmax)


if __name__ == '__main__':
    main()
