#!/bin/bash
#
# RAFT Intersection Benchmark Runner
# 
# Usage: ./benchmark.sh <vehicle_count> <result_name> [algorithm]
# 
# Arguments:
#   vehicle_count: 3, 4, 8, 16, or 32
#   result_name:   folder name for results
#   algorithm:     "raft" (default) or "greedy"
#
# Examples:
#   ./benchmark.sh 4 test_4veh           # Standard RAFT
#   ./benchmark.sh 8 test_8veh_greedy greedy  # Greedy RAFT
#
# Results will be saved in: benchmark/results/<result_name>/
#

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Paths
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_DIR="$SCRIPT_DIR/src"
RESULTS_BASE="$SCRIPT_DIR/results"

# NED path for OMNeT++
NED_PATH="..:..:../../src:../../../inet/src:../../../inet/examples:../../../inet/tutorials:../../../inet/showcases:../../../../veins/examples/veins:../../../../veins/src/veins"

# Check arguments
if [ $# -lt 2 ]; then
    echo -e "${RED}Usage: $0 <vehicle_count> <result_name> [num_iterations]${NC}"
    echo ""
    echo "Arguments:"
    echo "  vehicle_count:   4, 8, 16, or 32"
    echo "  result_name:     folder name for results"
    echo "  num_iterations:  number of runs (default: 10)"
    echo ""
    echo "Example:"
    echo "  $0 4 raft_4veh 10"
    exit 1
fi

VEHICLE_COUNT=$1
RESULT_NAME=$2
NUM_ITERATIONS=${3:-10}  # Default to 10 iterations
ALGORITHM="raft"  # RAFT only

# Validate vehicle count
if [[ ! "$VEHICLE_COUNT" =~ ^(4|8|16|32)$ ]]; then
    echo -e "${RED}Error: Vehicle count must be 4, 8, 16, or 32${NC}"
    exit 1
fi

SIM_DIR="$SCRIPT_DIR/simulations/intersection_$VEHICLE_COUNT"
if [ ! -d "$SIM_DIR" ]; then
    echo -e "${RED}Error: Directory $SIM_DIR not found. Run setup_scenarios.py first.${NC}"
    exit 1
fi

APP_TYPE="WillemtRaftApplication"

echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}  RAFT Multi-Run Benchmark${NC}"
echo -e "${GREEN}========================================${NC}"
echo ""
echo -e "Vehicles:    ${YELLOW}$VEHICLE_COUNT${NC}"
echo -e "Iterations:  ${YELLOW}$NUM_ITERATIONS${NC}"
echo -e "Result Name: ${YELLOW}$RESULT_NAME${NC}"
echo ""

# Create results directory
RESULT_DIR="$RESULTS_BASE/$RESULT_NAME"
mkdir -p "$RESULT_DIR"
echo -e "Results dir: ${YELLOW}$RESULT_DIR${NC}"
echo ""

# Multi-run loop
for iteration in $(seq 1 $NUM_ITERATIONS); do
    echo -e "${BLUE}========================================${NC}"
    echo -e "${BLUE}  Iteration $iteration / $NUM_ITERATIONS${NC}"
    echo -e "${BLUE}========================================${NC}"
    echo ""
    
    # Create iteration directory
    ITER_DIR="$RESULT_DIR/run_$iteration"
    mkdir -p "$ITER_DIR"

    RESULTS_JSON="$ITER_DIR/raft_results.json"

    # RAFT Timing (scaled by vehicle count)
    ELECTION_TIMEOUT_BASE=$((500 + VEHICLE_COUNT * 25))
    ELECTION_JITTER=$((100 + VEHICLE_COUNT * 50))
    REQUEST_TIMEOUT=$((100 + VEHICLE_COUNT * 5))
    STATUS_COLLECTION_TIMEOUT=$((300 + VEHICLE_COUNT * 20))

# Run simulation
echo -e "${GREEN}Running simulation (iteration $iteration)...${NC}"
echo ""

cd "$SIM_DIR"
LOG_FILE="$ITER_DIR/console.log"

# Build CLI overrides (no temp INI file needed)
CLI_OVERRIDES=(
    "--seed-set=$iteration"
    "--**.app[0].resultsFile=\"$RESULTS_JSON\""
    "--**.app[0].electionTimeoutBaseMs=$ELECTION_TIMEOUT_BASE"
    "--**.app[0].electionTimeoutJitterMs=$ELECTION_JITTER"
    "--**.app[0].requestTimeoutMs=$REQUEST_TIMEOUT"
    "--**.app[0].statusCollectionTimeoutMs=$STATUS_COLLECTION_TIMEOUT"
)

# Run and capture output (use release build to avoid library conflicts)
# If release doesn't exist, fall back to debug
if [ -x "$SRC_DIR/benchmark" ]; then
    "$SRC_DIR/benchmark" -u Cmdenv -n "$NED_PATH" omnetpp_udp.ini "${CLI_OVERRIDES[@]}" 2>&1 | tee "$LOG_FILE" > /dev/null
else
    "$SRC_DIR/benchmark_dbg" -u Cmdenv -n "$NED_PATH" omnetpp_udp.ini "${CLI_OVERRIDES[@]}" 2>&1 | tee "$LOG_FILE" > /dev/null
fi

# Check if results were generated
if [ ! -f "$RESULTS_JSON" ]; then
    echo -e "${YELLOW}Warning: Results file not generated for iteration $iteration${NC}"
fi

# Ensure JSON file is properly closed
if [ -f "$RESULTS_JSON" ]; then
    if ! grep -q '^\]$' "$RESULTS_JSON" && ! tail -c 5 "$RESULTS_JSON" | grep -q ']'; then
        echo -e "${YELLOW}Fixing incomplete JSON...${NC}"
        echo -e "\n]" >> "$RESULTS_JSON"
    fi
    
    # Generate timeline plot for this run
    echo -e "${YELLOW}Generating timeline plot...${NC}"
    python3 - "$RESULTS_JSON" "$ITER_DIR" << 'PLOT_SCRIPT'
import json
import sys
import matplotlib.pyplot as plt

results_file = sys.argv[1]
output_dir = sys.argv[2]

with open(results_file) as f:
    results = json.load(f)

if not results:
    sys.exit(0)

# Timeline plot
fig, ax = plt.subplots(figsize=(14, max(6, len(results) * 0.4)))

for v in results:
    vid = v['vehicle_id']
    ts = v['timestamps_ms']
    stopped = ts['stopped']
    passed = ts['passed']
    
    ax.barh(vid, passed - stopped, left=stopped, color='#2ecc71', alpha=0.7, height=0.6)
    
    if v.get('was_leader', False) and ts.get('elected', 0) > 0:
        ax.scatter([ts['elected']], [vid], color='blue', s=100, zorder=5, marker='*')

from matplotlib.patches import Patch
legend_elements = [
    Patch(facecolor='#2ecc71', alpha=0.7, label='Standard RAFT'),
    Patch(facecolor='#e74c3c', alpha=0.7, label='Fallback'),
    plt.Line2D([0], [0], marker='*', color='w', markerfacecolor='blue', markersize=10, label='Elected Leader')
]
ax.legend(handles=legend_elements, loc='upper right')

ax.set_xlabel('Time (ms)', fontsize=12)
ax.set_ylabel('Vehicle ID', fontsize=12)
ax.set_title(f'Standard RAFT Intersection Timeline ({len(results)} vehicles)', fontsize=14)
ax.set_yticks(range(len(results)))
ax.set_yticklabels([v['vehicle_id'] for v in results])
ax.grid(True, alpha=0.3, axis='x')

plt.tight_layout()
plt.savefig(f'{output_dir}/timeline.png', dpi=150, bbox_inches='tight')
plt.close()
PLOT_SCRIPT
fi

echo -e "${GREEN}Iteration $iteration complete${NC}"
echo ""

done  # End of iteration loop

# Cleanup temp files (do NOT delete base omnetpp_udp.ini / omnetpp_wave.ini)
# No temp files to clean up (using CLI overrides)

echo ""
echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}  Benchmark Complete!${NC}"
echo -e "${GREEN}========================================${NC}"
echo ""
echo -e "Results saved in: ${YELLOW}$RESULT_DIR${NC}"
echo -e "  - run_1/ through run_$NUM_ITERATIONS/  (individual runs)"
echo ""
echo "To analyze results, check the JSON files in each run directory."
echo ""

# ============ AGGREGATE ANALYSIS ============
echo ""
echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}  Generating Aggregate Analysis${NC}"
echo -e "${GREEN}========================================${NC}"
echo ""

python3 - <<AGGREGATE_SCRIPT
import json
import glob
import numpy as np
import matplotlib.pyplot as plt

result_dir = "$RESULT_DIR"
num_runs = $NUM_ITERATIONS

# Collect data from all runs
all_runs_data = []
for run_num in range(1, num_runs + 1):
    json_file = f"{result_dir}/run_{run_num}/raft_results.json"
    try:
        with open(json_file, 'r') as f:
            data = json.load(f)
            all_runs_data.append(data)
    except:
        print(f"Warning: Could not load {json_file}")

if not all_runs_data:
    print("No data found for aggregate analysis")
    exit(0)

# Calculate aggregate metrics
num_vehicles = len(all_runs_data[0])
metrics = {
    'raft_decision_time': [],  # Scenario-level: earliest order commit
    'total_intersection_time': [],  # Scenario-level: first stopped to last passed
    'throughput': [],
    'total_wait_time': [],
    'transit_time': [],
    'messages_sent': [],
    'messages_received': [],
    'election_rounds': []
}

for run_data in all_runs_data:
    # Filter for valid RAFT commits (exclude fallback)
    raft_commits = [v['timestamps_ms'].get('order_committed', 0) for v in run_data 
                   if v.get('coordination_method') != 'fallback' and v['timestamps_ms'].get('order_committed', 0) > 0]
    
    stopped_times = [v['timestamps_ms']['stopped'] for v in run_data]
    
    if raft_commits:
        latest_commit = max(raft_commits)
        first_stopped = min(stopped_times)
        raft_decision = latest_commit - first_stopped
    else:
        raft_decision = 0 
    
    # Store RAFT decision only if valid > 0
    if raft_decision > 0:
        metrics['raft_decision_time'].append(raft_decision)
    
    passed_times = [v['timestamps_ms']['passed'] for v in run_data]
    
    # Total Intersection Time = Last Passed - First Stopped
    start_time = min(stopped_times)
    end_time = max(passed_times)
    total_intersection = end_time - start_time
    metrics['total_intersection_time'].append(total_intersection)
    
    # Correct System Throughput: N_vehicles / (Total Time in seconds)
    if total_intersection > 0:
        system_throughput = num_vehicles / (total_intersection / 1000.0)
    else:
        system_throughput = 0
    metrics['throughput'].append(system_throughput)
    
    for vehicle in run_data:
        metrics['total_wait_time'].append(vehicle['durations_ms']['total_wait_time'])
        metrics['transit_time'].append(vehicle['durations_ms']['transit_time'])
        metrics['messages_sent'].append(vehicle['messages']['sent'])
        metrics['messages_received'].append(vehicle['messages']['received'])
        metrics['election_rounds'].append(vehicle.get('election_rounds', 0))

# Compute statistics
stats = {}
for key, values in metrics.items():
    stats[key] = {
        'mean': float(np.mean(values)),
        'std': float(np.std(values)),
        'min': float(np.min(values)),
        'max': float(np.max(values))
    }

# Save aggregate statistics
with open(f"{result_dir}/aggregate_stats.json", 'w') as f:
    json.dump(stats, f, indent=2)

print(f"Aggregate Statistics ({num_runs} runs, {num_vehicles} vehicles each):")
print(f"  RAFT Decision:   {stats['raft_decision_time']['mean']:.1f} ± {stats['raft_decision_time']['std']:.1f} ms")
print(f"  Total Intersect: {stats['total_intersection_time']['mean']:.1f} ± {stats['total_intersection_time']['std']:.1f} ms")
print(f"  Throughput:      {stats['throughput']['mean']:.3f} ± {stats['throughput']['std']:.3f} veh/s")
print(f"  Wait Time:       {stats['total_wait_time']['mean']:.1f} ± {stats['total_wait_time']['std']:.1f} ms")
print(f"  Transit Time:    {stats['transit_time']['mean']:.1f} ± {stats['transit_time']['std']:.1f} ms")
print(f"  Messages Sent:   {stats['messages_sent']['mean']:.1f} ± {stats['messages_sent']['std']:.1f}")
print(f"  Messages Recv:   {stats['messages_received']['mean']:.1f} ± {stats['messages_received']['std']:.1f}")
print(f"  Election Rounds: {stats['election_rounds']['mean']:.2f} ± {stats['election_rounds']['std']:.2f}")

# Calculate per-run averages for plotting
run_averages = {key: [] for key in metrics.keys()}
run_idx = 0
for run_data in all_runs_data:
    for key in metrics.keys():
        if key == 'raft_decision_time' or key == 'total_intersection_time':
            # These are already scenario-level (one value per run)
            # Just use the value we already calculated
            run_averages[key].append(metrics[key][run_idx])
        elif key == 'throughput':
            values = [v.get('throughput_veh_per_sec', 0) for v in run_data]
            run_averages[key].append(np.mean(values))
        elif key == 'total_wait_time':
            values = [v['durations_ms']['total_wait_time'] for v in run_data]
            run_averages[key].append(np.mean(values))
        elif key == 'transit_time':
            values = [v['durations_ms']['transit_time'] for v in run_data]
            run_averages[key].append(np.mean(values))
        elif key == 'messages_sent':
            values = [v['messages']['sent'] for v in run_data]
            run_averages[key].append(np.mean(values))
        elif key == 'messages_received':
            values = [v['messages']['received'] for v in run_data]
            run_averages[key].append(np.mean(values))
        elif key == 'election_rounds':
            values = [v.get('election_rounds', 0) for v in run_data]
            run_averages[key].append(np.mean(values))
    run_idx += 1

# Generate aggregate plots
fig, axes = plt.subplots(2, 4, figsize=(20, 10))
fig.suptitle(f'Aggregate Analysis: {num_runs} Runs × {num_vehicles} Vehicles', fontsize=16, fontweight='bold')

plot_configs = [
    ('raft_decision_time', 'Time (ms)', 'RAFT Decision Time'),
    ('total_intersection_time', 'Time (ms)', 'Total Intersection Time'),
    ('throughput', 'Throughput (veh/s)', 'Throughput'),
    ('total_wait_time', 'Wait Time (ms)', 'Total Wait Time'),
    ('transit_time', 'Transit Time (ms)', 'Transit Time'),
    ('messages_sent', 'Messages', 'Messages Sent'),
    ('messages_received', 'Messages', 'Messages Received'),
    ('election_rounds', 'Rounds', 'Election Rounds')
]

iterations = list(range(1, num_runs + 1))

for idx, (key, ylabel, title) in enumerate(plot_configs):
    ax = axes[idx // 4, idx % 4]
    
    # Bar plot
    bars = ax.bar(iterations, run_averages[key], color='#3498db', alpha=0.8, edgecolor='black', linewidth=0.5)
    
    # Add value labels on top of bars
    for i, (iter_num, value) in enumerate(zip(iterations, run_averages[key])):
        ax.text(iter_num, value, f'{value:.1f}', ha='center', va='bottom', fontsize=9, fontweight='bold')
    
    # Mean line
    ax.axhline(stats[key]['mean'], color='red', linestyle='--', linewidth=2, alpha=0.7, label=f"Mean: {stats[key]['mean']:.2f}")
    
    ax.set_xlabel('Iteration', fontsize=11, fontweight='bold')
    ax.set_ylabel(ylabel, fontsize=11, fontweight='bold')
    ax.set_title(title, fontsize=12, fontweight='bold')
    ax.set_xticks(iterations)
    ax.grid(True, alpha=0.3, axis='y')
    ax.legend(loc='best')

plt.tight_layout()
plt.savefig(f'{result_dir}/aggregate_analysis.png', dpi=150, bbox_inches='tight')
plt.close()

print(f"\nAggregate plots saved to: {result_dir}/aggregate_analysis.png")
print(f"Statistics saved to: {result_dir}/aggregate_stats.json")
AGGREGATE_SCRIPT

echo ""
echo -e "${GREEN}Aggregate analysis complete!${NC}"
echo ""
