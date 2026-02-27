#!/bin/bash
#
# Run simple intersection benchmark: UDP and WAVE for 4, 8, and 16 vehicles.
# Results saved to results/simple_udp_Nveh and results/simple_raftwave_Nveh.
#
# Prerequisite: veins_launchd must be running (SUMO started by it)
#   python3 /path/to/veins/bin/veins_launchd -vv -c sumo-gui
#
# Usage: ./run_simple_benchmark.sh [num_iterations]
#   num_iterations: number of runs per scenario (default: 3)

set -e

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_DIR="$SCRIPT_DIR/src"
RESULTS_BASE="$SCRIPT_DIR/results"

# NED path - from sim dir, .. = simulations (finds simple_intersection package)
NED_PATH="..:..:../../src:../../../inet/src:../../../inet/examples:../../../inet/tutorials:../../../inet/showcases:../../../../veins/examples/veins:../../../../veins/src/veins"

NUM_ITERATIONS=${1:-3}

# Vehicle count -> sim directory
get_sim_dir() {
    case $1 in
        4)  echo "$SCRIPT_DIR/simulations/simple_intersection_4" ;;
        8)  echo "$SCRIPT_DIR/simulations/simple_intersection_8" ;;
        16) echo "$SCRIPT_DIR/simulations/simple_intersection" ;;
        *)  echo "" ;;
    esac
}

run_udp() {
    local vc=$1
    local sim_dir=$(get_sim_dir $vc)
    local result_name="simple_udp_${vc}veh"
    local result_dir="$RESULTS_BASE/$result_name"
    mkdir -p "$result_dir"

    echo -e "${GREEN}--- UDP ${vc} vehicles ---${NC}"
    for i in $(seq 1 $NUM_ITERATIONS); do
        local iter_dir="$result_dir/run_$i"
        mkdir -p "$iter_dir"
        local results_json="$iter_dir/raft_results.json"

        cd "$sim_dir"
        "$SRC_DIR/benchmark" -u Cmdenv -n "$NED_PATH" omnetpp_udp.ini \
            --seed-set=$i \
            "--**.app[0].resultsFile=\"$results_json\"" \
            2>&1 | tail -5

        if [ -f "raft_results.json" ] && [ ! -f "$results_json" ]; then
            mv "raft_results.json" "$results_json"
        fi
        echo "  Run $i done"
    done
}

run_wave() {
    local vc=$1
    local sim_dir=$(get_sim_dir $vc)
    local result_name="simple_raftwave_${vc}veh"
    local result_dir="$RESULTS_BASE/$result_name"
    mkdir -p "$result_dir"

    echo -e "${GREEN}--- WAVE ${vc} vehicles ---${NC}"
    for i in $(seq 1 $NUM_ITERATIONS); do
        local iter_dir="$result_dir/run_$i"
        mkdir -p "$iter_dir"
        local results_json="$iter_dir/raft_results.json"

        cd "$sim_dir"
        "$SRC_DIR/benchmark" -u Cmdenv -n "$NED_PATH" omnetpp_wave.ini \
            --seed-set=$i \
            "--**.appl.resultsFile=\"$results_json\"" \
            2>&1 | tail -5

        if [ -f "raft_results.json" ] && [ ! -f "$results_json" ]; then
            mv "raft_results.json" "$results_json"
        fi
        echo "  Run $i done"
    done
}

# Check build
if [ ! -x "$SRC_DIR/benchmark" ]; then
    echo -e "${RED}Error: Build benchmark first: cd src && make${NC}"
    exit 1
fi

echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}  Simple Intersection Benchmark${NC}"
echo -e "${GREEN}  UDP + WAVE for 4, 8, 16 vehicles${NC}"
echo -e "${GREEN}  $NUM_ITERATIONS iterations each${NC}"
echo -e "${GREEN}========================================${NC}"
echo ""

for vc in 4 8 16; do
    sim_dir=$(get_sim_dir $vc)
    if [ ! -d "$sim_dir" ]; then
        echo -e "${RED}Error: $sim_dir not found${NC}"
        exit 1
    fi
done

for vc in 4 8 16; do
    run_udp $vc
    run_wave $vc
done

echo ""
echo -e "${GREEN}All runs complete. Generating aggregate stats and plots...${NC}"
echo ""

# Run aggregation for each result folder
for result_name in simple_udp_4veh simple_udp_8veh simple_udp_16veh simple_raftwave_4veh simple_raftwave_8veh simple_raftwave_16veh; do
    result_dir="$RESULTS_BASE/$result_name"
    if [ -d "$result_dir" ]; then
        python3 - "$result_dir" "$NUM_ITERATIONS" 2>/dev/null << 'AGGSCRIPT' || true
import json
import sys
import os
import numpy as np

result_dir = sys.argv[1]
num_runs = int(sys.argv[2])

all_data = []
for run_num in range(1, num_runs + 1):
    json_file = os.path.join(result_dir, f'run_{run_num}', 'raft_results.json')
    if os.path.exists(json_file):
        try:
            with open(json_file) as f:
                raw = f.read()
            data = json.loads(raw)
            if data:
                all_data.append(data)
        except json.JSONDecodeError:
            # Repair truncated JSON (missing closing ])
            try:
                s = raw.rstrip()
                if s and not s.rstrip().endswith(']'):
                    if s.rstrip().endswith('}'):
                        data = json.loads(s + '\n]')
                    else:
                        data = json.loads(s.rstrip().rstrip(',') + '\n]')
                    if data:
                        all_data.append(data)
            except Exception:
                pass
        except Exception:
            pass

if not all_data:
    sys.exit(0)

num_vehicles = len(all_data[0])
metrics = {'raft_decision_time': [], 'total_intersection_time': [], 'throughput': [], 'total_wait_time': [], 'transit_time': [], 'fallback_rate': []}

for run_data in all_data:
    raft_commits = [v['timestamps_ms'].get('order_committed', 0) for v in run_data if v.get('coordination_method') != 'fallback' and v['timestamps_ms'].get('order_committed', 0) > 0]
    cluster_times = [v['timestamps_ms'].get('cluster_formed', 0) for v in run_data if v['timestamps_ms'].get('cluster_formed', 0) > 0]
    if raft_commits and cluster_times:
        rd = max(raft_commits) - min(cluster_times)
        if rd > 0:
            metrics['raft_decision_time'].append(rd)
    stopped = [v['timestamps_ms']['stopped'] for v in run_data]
    passed = [v['timestamps_ms']['passed'] for v in run_data]
    if stopped and passed:
        total_time = max(passed) - min(stopped)
        metrics['total_intersection_time'].append(total_time)
    # Throughput: N / (last RAFT vehicle passed - first RAFT start)
    raft_vehicles = [v for v in run_data if v.get('coordination_method') != 'fallback']
    raft_starts = [v['timestamps_ms'].get('cluster_formed', 0) for v in raft_vehicles if v['timestamps_ms'].get('cluster_formed', 0) > 0]
    raft_passed = [v['timestamps_ms']['passed'] for v in raft_vehicles]
    if raft_starts and raft_passed:
        raft_time_ms = max(raft_passed) - min(raft_starts)
        if raft_time_ms > 0:
            n_raft = len(raft_vehicles)
            metrics['throughput'].append(n_raft / (raft_time_ms / 1000.0))
    fallback_count = sum(1 for v in run_data if v.get('coordination_method') == 'fallback')
    metrics['fallback_rate'].append(fallback_count / num_vehicles if num_vehicles > 0 else 0)
    for v in run_data:
        metrics['total_wait_time'].append(v['durations_ms']['total_wait_time'])
        metrics['transit_time'].append(v['durations_ms']['transit_time'])

agg = {}
for k, vals in metrics.items():
    agg[k] = {'mean': float(np.mean(vals)), 'std': float(np.std(vals))} if vals else {'mean': 0, 'std': 0}

with open(os.path.join(result_dir, 'aggregate_stats.json'), 'w') as f:
    json.dump(agg, f, indent=2)
AGGSCRIPT
    fi
done

# Run plot
echo -e "${GREEN}Running plot_comparison.py --simple...${NC}"
cd "$SCRIPT_DIR"
python3 plot_comparison.py --simple

echo ""
echo -e "${GREEN}Done. Results in results/simple_udp_* and results/simple_raftwave_*${NC}"
echo -e "Plots: results/simple_wave_vs_udp_comparison.png"
