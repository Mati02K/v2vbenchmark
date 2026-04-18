#!/bin/bash
# Simple intersection benchmark: UDP + WAVE for 4, 8, 16 vehicles.
# Usage: ./run_simple_benchmark.sh [iterations] [cluster_mode] [nopriority]
#   cluster_mode : laneLeaders (default) | allVehicles
#   nopriority   : omit to rotate a priority vehicle each run

set -e

GREEN='\033[0;32m'; RED='\033[0;31m'; NC='\033[0m'

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_DIR="$SCRIPT_DIR/src"
RESULTS_BASE="$SCRIPT_DIR/results"
NED_PATH="..:..:../../src:../../../inet/src:../../../inet/examples:../../../inet/tutorials:../../../inet/showcases:../../../../veins/examples/veins:../../../../veins/src/veins"

NUM_ITERATIONS=${1:-3}
CLUSTER_MODE=${2:-laneLeaders}
NO_PRIORITY=${3:-}   # set to "nopriority" to disable rotating priority vehicle

# Result directories get a _nopriority suffix when priority is disabled,
# so priority and nopriority runs never overwrite each other.
RESULT_MODE="${CLUSTER_MODE}$( [ "$NO_PRIORITY" = "nopriority" ] && echo "_nopriority" || echo "" )"

get_sim_dir() {
    case $1 in
        4)  echo "$SCRIPT_DIR/simulations/simple_intersection_4" ;;
        8)  echo "$SCRIPT_DIR/simulations/simple_intersection_8" ;;
        16) echo "$SCRIPT_DIR/simulations/simple_intersection" ;;
    esac
}

make_prio_ini() {
    local transport=$1 vc=$2 run=$3
    local prio_ini="/tmp/prio_$$.ini"
    if [ "$NO_PRIORITY" = "nopriority" ]; then
        printf '[General]\n**.isPriorityVehicle = false\n' > "$prio_ini"
    else
        local vid=$(( (run - 1) % vc ))
        local app_key=$( [ "$transport" = "udp" ] && echo "app[0]" || echo "appl" )
        printf '[General]\n**.node[%d].%s.isPriorityVehicle = true\n**.isPriorityVehicle = false\n' \
            "$vid" "$app_key" > "$prio_ini"
        echo "  Run $run: priority vehicle = V${vid}" >&2
    fi
    echo "$prio_ini"
}

run_scenario() {
    local transport=$1 vc=$2
    local ini result_prefix app_key
    if [ "$transport" = "udp" ]; then
        ini="omnetpp_udp.ini"; result_prefix="simple_udp"; app_key="app[0]"
    else
        ini="omnetpp_wave.ini"; result_prefix="simple_raftwave"; app_key="appl"
    fi

    local result_dir="$RESULTS_BASE/${result_prefix}_${vc}veh_${RESULT_MODE}"
    local sim_dir=$(get_sim_dir $vc)
    mkdir -p "$result_dir"

    echo -e "${GREEN}--- ${transport^^} ${vc} vehicles (${CLUSTER_MODE}) ---${NC}"
    for i in $(seq 1 $NUM_ITERATIONS); do
        local iter_dir="$result_dir/run_$i"
        mkdir -p "$iter_dir"
        local results_json="$iter_dir/raft_results.json"
        local prio_ini=$(make_prio_ini "$transport" "$vc" "$i")

        cd "$sim_dir"
        "$SRC_DIR/benchmark" -u Cmdenv -n "$NED_PATH" "$ini" -f "$prio_ini" \
            --seed-set=$i \
            "--**.${app_key}.resultsFile=\"$results_json\"" \
            "--**.${app_key}.clusterMode=\"$CLUSTER_MODE\"" \
            > "$iter_dir/console.log" 2>&1
        rm -f "$prio_ini"

        [ -f "raft_results.json" ] && [ ! -f "$results_json" ] && mv "raft_results.json" "$results_json"
        echo "  Run $i done"
    done
}

aggregate_results() {
    local result_dir=$1
    [ -d "$result_dir" ] || return
    python3 - "$result_dir" "$NUM_ITERATIONS" 2>/dev/null << 'PY' || true
import json, sys, os, numpy as np

result_dir, num_runs = sys.argv[1], int(sys.argv[2])
all_data = []
for n in range(1, num_runs + 1):
    path = os.path.join(result_dir, f'run_{n}', 'raft_results.json')
    if not os.path.exists(path): continue
    try:
        raw = open(path).read()
        data = json.loads(raw)
    except json.JSONDecodeError:
        s = raw.rstrip()
        try: data = json.loads((s + '\n]') if s.endswith('}') else (s.rstrip(',') + '\n]'))
        except: continue
    except: continue
    if data: all_data.append(data)

if not all_data: sys.exit(0)

metrics = {k: [] for k in ('raft_decision_time', 'total_intersection_time',
                            'throughput', 'total_wait_time', 'transit_time', 'fallback_rate')}
nv = len(all_data[0])
for run in all_data:
    raft = [v for v in run if v.get('coordination_method') != 'fallback']
    rd = [v['durations_ms'].get('raft_decision_time', 0) for v in raft if v['durations_ms'].get('raft_decision_time', 0) > 0]
    if rd: metrics['raft_decision_time'].append(sum(rd))
    stopped = [v['timestamps_ms']['stopped'] for v in run]
    passed  = [v['timestamps_ms']['passed']  for v in run]
    if stopped and passed: metrics['total_intersection_time'].append(max(passed) - min(stopped))
    rp = [v['timestamps_ms']['passed'] for v in raft if v['timestamps_ms'].get('passed', 0) > 0]
    if rp:
        w = max(rp) - min(rp)
        if w > 0: metrics['throughput'].append(len(raft) / (w / 1000.0))
    metrics['fallback_rate'].append(sum(1 for v in run if v.get('coordination_method') == 'fallback') / nv)
    for v in run:
        metrics['total_wait_time'].append(v['durations_ms']['total_wait_time'])
        metrics['transit_time'].append(v['durations_ms']['transit_time'])

agg = {k: {'mean': float(np.mean(v)), 'std': float(np.std(v))} if v else {'mean': 0, 'std': 0}
       for k, v in metrics.items()}
json.dump(agg, open(os.path.join(result_dir, 'aggregate_stats.json'), 'w'), indent=2)
PY
}

# Checks
[ -x "$SRC_DIR/benchmark" ] || { echo -e "${RED}Error: build first: cd src && make${NC}"; exit 1; }
for vc in 4 8 16; do
    [ -d "$(get_sim_dir $vc)" ] || { echo -e "${RED}Error: sim dir for ${vc}veh not found${NC}"; exit 1; }
done

echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}  Simple Intersection Benchmark${NC}"
echo -e "${GREEN}  UDP + WAVE for 4, 8, 16 vehicles${NC}"
echo -e "${GREEN}  $NUM_ITERATIONS iterations | mode: ${RESULT_MODE} | priority: ${NO_PRIORITY:-enabled}${NC}"
echo -e "${GREEN}========================================${NC}"

for vc in 4 8 16; do
    run_scenario udp  $vc
    run_scenario wave $vc
done

echo -e "\n${GREEN}All runs complete. Aggregating and plotting...${NC}\n"

for prefix in simple_udp simple_raftwave; do
    for vc in 4 8 16; do
        aggregate_results "$RESULTS_BASE/${prefix}_${vc}veh_${RESULT_MODE}"
    done
done

cd "$SCRIPT_DIR"
echo -e "${GREEN}Running plot_comparison.py --simple --mode ${RESULT_MODE}...${NC}"
python3 plot_comparison.py --simple --mode "$RESULT_MODE"

echo -e "\n${GREEN}Done. Results in results/*_${RESULT_MODE}${NC}"
