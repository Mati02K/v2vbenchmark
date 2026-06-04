"""
Comparison script: RAFT (allVehicles WAVE) vs PBFT (allVehicles WAVE).
Generates 5 plots: decision latency, wait time (throughput proxy), CDF, priority effect, throughput.
"""

import json
import glob
import statistics
import os
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

RESULTS_DIR    = os.path.join(os.path.dirname(__file__), "results")
PBFT_DIR       = os.path.join(os.path.dirname(__file__),
                 "DistributedSystemsforAVs", "benchmarks")
OUT_DIR        = os.path.join(os.path.dirname(__file__), "results")
VEHICLE_COUNTS = [4, 8, 16, 20]

RAFT_COLOR       = "#2196F3"   # blue
RAFT_PRIO_COLOR  = "#1565C0"   # dark blue
PBFT_COLOR       = "#F44336"   # red
PBFT_PRIO_COLOR  = "#B71C1C"   # dark red

# ── loaders ────────────────────────────────────────────────────────────────────

def load_raft_runs(n_veh, priority=True):
    suffix = "" if priority else "_nopriority"
    pattern = f"{RESULTS_DIR}/simple_raftwave_{n_veh}veh_allVehicles{suffix}/run_*/raft_results.json"
    vehicles = []
    for path in sorted(glob.glob(pattern)):
        try:
            with open(path) as f:
                data = json.load(f)
            rows = data if isinstance(data, list) else data.get("vehicles", [])
            vehicles.extend(rows)
        except Exception as e:
            print(f"WARNING: {path}: {e}")
    return vehicles


def load_pbft_runs(n_veh, priority=True):
    scenario = "amb_honest" if priority else "no_amb"
    pattern = f"{PBFT_DIR}/Priority{n_veh}cars/{scenario}/run_*/{n_veh}veh_0.json"
    vehicles = []
    for path in sorted(glob.glob(pattern)):
        try:
            with open(path) as f:
                data = json.load(f)
            rows = data if isinstance(data, list) else []
            vehicles.extend(rows)
        except Exception as e:
            print(f"WARNING: {path}: {e}")
    return vehicles


def avg_metric(vehicles, key):
    vals = []
    for v in vehicles:
        if v.get("coordination_method") == "fallback":
            continue
        dur = v.get("durations_ms", {})
        val = dur.get(key) if key in dur else v.get(key)
        if val is not None and val > 0:
            vals.append(val)
    return statistics.mean(vals) if vals else 0.0


def collect_wait_times(vehicles):
    vals = []
    for v in vehicles:
        if v.get("coordination_method") == "fallback":
            continue
        wt = v.get("durations_ms", {}).get("total_wait_time")
        if wt is not None and wt > 0:
            vals.append(wt)
    return vals


def split_priority(vehicles):
    prio   = [v for v in vehicles if v.get("is_priority_vehicle")]
    normal = [v for v in vehicles if not v.get("is_priority_vehicle")]
    return prio, normal


def compute_throughput(vehicles, n_veh):
    """Vehicles per second through the intersection per run (excludes fallbacks)."""
    raft_vehicles = [v for v in vehicles if v.get("coordination_method") != "fallback"]
    stopped_times = [v.get("timestamps_ms", {}).get("stopped") for v in raft_vehicles]
    passed_times  = [v.get("timestamps_ms", {}).get("passed")  for v in raft_vehicles]
    stopped_times = [t for t in stopped_times if t is not None and t > 0]
    passed_times  = [t for t in passed_times  if t is not None and t > 0]
    if not stopped_times or not passed_times:
        return 0.0
    total_sec = (max(passed_times) - min(stopped_times)) / 1000.0
    if total_sec <= 0:
        return 0.0
    return len(passed_times) / total_sec


def throughput_per_run(n_veh, load_fn, priority):
    """Return one throughput value per run file."""
    suffix = "" if priority else "_nopriority"
    if load_fn == load_raft_runs:
        pattern = f"{RESULTS_DIR}/simple_raftwave_{n_veh}veh_allVehicles{suffix}/run_*/raft_results.json"
    else:
        scenario = "amb_honest" if priority else "no_amb"
        pattern  = f"{PBFT_DIR}/Priority{n_veh}cars/{scenario}/run_*/{n_veh}veh_0.json"

    run_throughputs = []
    for path in sorted(glob.glob(pattern)):
        try:
            with open(path) as f:
                data = json.load(f)
            rows = data if isinstance(data, list) else data.get("vehicles", [])
            tp = compute_throughput(rows, n_veh)
            if tp > 0:
                run_throughputs.append(tp)
        except Exception as e:
            print(f"WARNING: {path}: {e}")
    return statistics.mean(run_throughputs) if run_throughputs else 0.0


# ── collect data ───────────────────────────────────────────────────────────────

raft_decision,    pbft_decision    = [], []
raft_wait,        pbft_wait        = [], []
raft_throughput,  pbft_throughput  = [], []
raft_prio_wait,   raft_norm_wait   = [], []
pbft_prio_wait,   pbft_norm_wait   = [], []
raft_all_waits,   pbft_all_waits   = [], []

for n in VEHICLE_COUNTS:
    raft_np = load_raft_runs(n, priority=False)
    pbft_np = load_pbft_runs(n, priority=False)
    raft_pr = load_raft_runs(n, priority=True)
    pbft_pr = load_pbft_runs(n, priority=True)

    raft_decision.append(avg_metric(raft_np, "decision_latency_ms"))
    pbft_decision.append(avg_metric(pbft_np, "decision_latency_ms"))
    raft_wait.append(avg_metric(raft_np, "total_wait_time"))
    pbft_wait.append(avg_metric(pbft_np, "total_wait_time"))

    raft_throughput.append(throughput_per_run(n, load_raft_runs, priority=False))
    pbft_throughput.append(throughput_per_run(n, load_pbft_runs, priority=False))

    rp, rn = split_priority(raft_pr)
    pp, pn = split_priority(pbft_pr)
    raft_prio_wait.append(avg_metric(rp, "total_wait_time"))
    raft_norm_wait.append(avg_metric(rn, "total_wait_time"))
    pbft_prio_wait.append(avg_metric(pp, "total_wait_time"))
    pbft_norm_wait.append(avg_metric(pn, "total_wait_time"))

    raft_all_waits.extend(collect_wait_times(raft_np))
    pbft_all_waits.extend(collect_wait_times(pbft_np))

# ── plot helpers ───────────────────────────────────────────────────────────────

X      = np.arange(len(VEHICLE_COUNTS))
WIDTH  = 0.35
LABELS = [f"{n} veh" for n in VEHICLE_COUNTS]


def style_ax(ax, title, ylabel, xlabel="Number of Vehicles"):
    ax.set_title(title, fontsize=13, fontweight="bold", pad=10)
    ax.set_ylabel(ylabel, fontsize=11)
    ax.set_xlabel(xlabel, fontsize=11)
    ax.set_xticks(X)
    ax.set_xticklabels(LABELS)
    ax.legend(fontsize=10)
    ax.grid(axis="y", alpha=0.3)
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)


# ── Plot 1: Decision Latency ───────────────────────────────────────────────────

x = np.arange(len(VEHICLE_COUNTS))
fig, ax = plt.subplots(figsize=(8, 5))
ax.plot(VEHICLE_COUNTS, raft_decision, color=RAFT_COLOR, linewidth=2, marker='o',
        markersize=6, label="RAFT (WAVE)")
ax.plot(VEHICLE_COUNTS, pbft_decision, color=PBFT_COLOR, linewidth=2, marker='o',
        markersize=6, label="PBFT (WAVE)")
ax.set_title("Consensus Decision Latency: RAFT vs PBFT", fontsize=13, fontweight="bold", pad=10)
ax.set_ylabel("Avg Decision Latency (ms)", fontsize=11)
ax.set_xlabel("Number of Vehicles", fontsize=11)
ax.set_xticks(VEHICLE_COUNTS)
ax.set_xticklabels([f"{n} veh" for n in VEHICLE_COUNTS])
ax.set_ylim(bottom=0)
ax.legend(fontsize=10)
ax.grid(axis="y", alpha=0.3)
ax.spines["top"].set_visible(False)
ax.spines["right"].set_visible(False)
plt.tight_layout()
plt.savefig(os.path.join(OUT_DIR, "comparison_decision_latency.png"), dpi=150)
plt.close()
print("  Saved: comparison_decision_latency.png")

# ── Plot 2: Total Wait Time ────────────────────────────────────────────────────

fig, ax = plt.subplots(figsize=(8, 5))
ax.bar(X - WIDTH/2, [w/1000 for w in raft_wait], WIDTH, label="RAFT (WAVE)", color=RAFT_COLOR)
ax.bar(X + WIDTH/2, [w/1000 for w in pbft_wait], WIDTH, label="PBFT (WAVE)", color=PBFT_COLOR)
style_ax(ax, "Total Intersection Wait Time: RAFT vs PBFT",
         "Avg Wait Time (s)  [stopped → passed]")
plt.tight_layout()
plt.savefig(os.path.join(OUT_DIR, "comparison_wait_time.png"), dpi=150)
plt.close()
print("  Saved: comparison_wait_time.png")

# ── Plot 3: Throughput ─────────────────────────────────────────────────────────

fig, ax = plt.subplots(figsize=(8, 5))
ax.plot(VEHICLE_COUNTS, raft_throughput, color=RAFT_COLOR, linewidth=2, marker='o',
        markersize=6, label="RAFT (WAVE)")
ax.plot(VEHICLE_COUNTS, pbft_throughput, color=PBFT_COLOR, linewidth=2, marker='o',
        markersize=6, label="PBFT (WAVE)")
ax.set_title("Intersection Throughput: RAFT vs PBFT", fontsize=13, fontweight="bold", pad=10)
ax.set_ylabel("Throughput (vehicles / second)", fontsize=11)
ax.set_xlabel("Number of Vehicles", fontsize=11)
ax.set_xticks(VEHICLE_COUNTS)
ax.set_xticklabels([f"{n} veh" for n in VEHICLE_COUNTS])
ax.set_ylim(bottom=0)
ax.legend(fontsize=10)
ax.grid(axis="y", alpha=0.3)
ax.spines["top"].set_visible(False)
ax.spines["right"].set_visible(False)
plt.tight_layout()
plt.savefig(os.path.join(OUT_DIR, "comparison_throughput.png"), dpi=150)
plt.close()
print("  Saved: comparison_throughput.png")

# ── Plot 4: CDF of Wait Times (all vehicles pooled) ───────────────────────────

fig, ax = plt.subplots(figsize=(9, 5))

def plot_cdf(ax, data, color, label):
    if not data:
        return
    sorted_data = sorted(d / 1000 for d in data)
    y = np.arange(1, len(sorted_data) + 1) / len(sorted_data)
    ax.plot(sorted_data, y, color=color, linewidth=2, label=label)

plot_cdf(ax, raft_all_waits, RAFT_COLOR, "RAFT (WAVE)")
plot_cdf(ax, pbft_all_waits, PBFT_COLOR, "PBFT (WAVE)")
ax.set_title("CDF of Vehicle Wait Times: RAFT vs PBFT", fontsize=13, fontweight="bold", pad=10)
ax.set_xlabel("Wait Time (s)  [stopped → passed]", fontsize=11)
ax.set_ylabel("Cumulative Fraction of Vehicles", fontsize=11)
ax.set_ylim(0, 1.05)
ax.set_xlim(left=0)
ax.legend(fontsize=10)
ax.grid(alpha=0.3)
ax.spines["top"].set_visible(False)
ax.spines["right"].set_visible(False)
plt.tight_layout()
plt.savefig(os.path.join(OUT_DIR, "comparison_cdf.png"), dpi=150)
plt.close()
print("  Saved: comparison_cdf.png")

# ── Plot 5: Priority Effect ────────────────────────────────────────────────────

fig, ax = plt.subplots(figsize=(9, 5))
w = 0.2
ax.bar(X - 1.5*w, [v/1000 for v in raft_prio_wait], w,
       label="RAFT — priority veh",  color=RAFT_PRIO_COLOR)
ax.bar(X - 0.5*w, [v/1000 for v in raft_norm_wait], w,
       label="RAFT — normal veh",    color=RAFT_COLOR,      alpha=0.8)
ax.bar(X + 0.5*w, [v/1000 for v in pbft_prio_wait], w,
       label="PBFT — priority veh",  color=PBFT_PRIO_COLOR)
ax.bar(X + 1.5*w, [v/1000 for v in pbft_norm_wait], w,
       label="PBFT — normal veh",    color=PBFT_COLOR,      alpha=0.8)
style_ax(ax, "Priority Vehicle Benefit: RAFT vs PBFT",
         "Avg Wait Time (s)  [stopped → passed]")
plt.tight_layout()
plt.savefig(os.path.join(OUT_DIR, "comparison_priority.png"), dpi=150)
plt.close()
print("  Saved: comparison_priority.png")

print(f"\nAll plots saved to {OUT_DIR}/")
