#!/bin/bash
# Simple intersection benchmark with priority rotation.
# WAVE runs for all modes; UDP runs only for allVehicles.
#
# Usage: ./benchmark.sh [iterations] [cluster_mode] [multirounds]
#
#   iterations   : number of runs per scenario (default: 3)
#   cluster_mode : laneLeaders (default) | allVehicles
#                  NOTE: ignored when multirounds is set — forced to allVehicles
#   multirounds  : pass "multirounds" to allow extra RAFT rounds even when all
#                  vehicles are already scheduled; forces cluster_mode=allVehicles
#
# Valid combinations:
#   ./benchmark.sh 3 laneLeaders    # WAVE only
#   ./benchmark.sh 3 allVehicles    # WAVE + UDP
#   ./benchmark.sh 3 "" multirounds # WAVE only

set -e

GREEN='\033[0;32m'; RED='\033[0;31m'; NC='\033[0m'

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_DIR="$SCRIPT_DIR/src"
RESULTS_BASE="$SCRIPT_DIR/results"
NED_PATH="..:..:../../src:../../../inet/src:../../../inet/examples:../../../inet/tutorials:../../../inet/showcases:../../../../veins/examples/veins:../../../../veins/src/veins"

NUM_ITERATIONS=${1:-3}
CLUSTER_MODE=${2:-laneLeaders}
MULTI_ROUNDS=${3:-}  # set to "multirounds" to enable allowMultipleRounds

# multirounds requires allVehicles so all present vehicles can participate in round 2+
if [ "$MULTI_ROUNDS" = "multirounds" ]; then
    [ -n "${2:-}" ] && [ "${2:-}" != "allVehicles" ] && \
        echo -e "${RED}Warning: cluster_mode='${2}' ignored — multirounds forces allVehicles${NC}"
    CLUSTER_MODE="allVehicles"
fi

RESULT_MODE="${CLUSTER_MODE}$( [ "$MULTI_ROUNDS" = "multirounds" ] && echo "_multirounds" || echo "" )"

get_sim_dir() {
    echo "$SCRIPT_DIR/simulations/simple_intersection"
}

make_prio_ini() {
    local transport=$1 vc=$2 run=$3
    local prio_ini="/tmp/prio_$$.ini"
    local vid=$(( (run - 1) % vc ))
    local app_key=$( [ "$transport" = "udp" ] && echo "app[0]" || echo "appl" )
    printf '[General]\n**.node[%d].%s.isPriorityVehicle = true\n**.isPriorityVehicle = false\n' \
        "$vid" "$app_key" > "$prio_ini"
    echo "  Run $run: priority vehicle = V${vid}" >&2
    echo "$prio_ini"
}

make_noprio_ini() {
    local prio_ini="/tmp/prio_$$.ini"
    printf '[General]\n**.isPriorityVehicle = false\n' > "$prio_ini"
    echo "$prio_ini"
}

run_scenario() {
    local transport=$1 vc=$2 nopriority=${3:-}
    local ini result_prefix app_key
    if [ "$transport" = "udp" ]; then
        ini="omnetpp_udp.ini"; result_prefix="simple_udp"; app_key="app[0]"
    else
        ini="omnetpp_wave.ini"; result_prefix="simple_raftwave"; app_key="appl"
    fi

    local dir_suffix="${RESULT_MODE}$( [ "$nopriority" = "nopriority" ] && echo "_nopriority" || echo "" )"
    local result_dir="$RESULTS_BASE/${result_prefix}_${vc}veh_${dir_suffix}"
    local sim_dir=$(get_sim_dir)
    mkdir -p "$result_dir"

    local prio_label=$( [ "$nopriority" = "nopriority" ] && echo "no priority" || echo "priority rotating" )
    echo -e "${GREEN}--- ${transport^^} ${vc} vehicles (${CLUSTER_MODE}, ${prio_label}) ---${NC}"
    for i in $(seq 1 $NUM_ITERATIONS); do
        local iter_dir="$result_dir/run_$i"
        mkdir -p "$iter_dir"
        local results_json="$iter_dir/raft_results.json"
        local prio_ini
        if [ "$nopriority" = "nopriority" ]; then
            prio_ini=$(make_noprio_ini)
        else
            prio_ini=$(make_prio_ini "$transport" "$vc" "$i")
        fi

        cd "$sim_dir"
        local allow_multi=$( [ "$MULTI_ROUNDS" = "multirounds" ] && echo "true" || echo "false" )
        "$SRC_DIR/benchmark" -u Cmdenv -n "$NED_PATH" "$ini" -c "Veh${vc}" -f "$prio_ini" \
            --seed-set=$i \
            "--**.${app_key}.resultsFile=\"$results_json\"" \
            "--**.${app_key}.clusterMode=\"$CLUSTER_MODE\"" \
            "--**.${app_key}.allowMultipleRounds=$allow_multi" \
            > "$iter_dir/console.log" 2>&1
        rm -f "$prio_ini"

        [ -f "raft_results.json" ] && [ ! -f "$results_json" ] && mv "raft_results.json" "$results_json"
        echo "  Run $i done"
    done
}

# Checks
[ -x "$SRC_DIR/benchmark" ] || { echo -e "${RED}Error: build first: cd src && make${NC}"; exit 1; }
[ -d "$(get_sim_dir)" ] || { echo -e "${RED}Error: sim dir not found: $(get_sim_dir)${NC}"; exit 1; }

echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}  Simple Intersection Benchmark${NC}"
echo -e "${GREEN}  WAVE (all modes) + UDP (allVehicles only) for 4, 8, 16 vehicles${NC}"
echo -e "${GREEN}  $NUM_ITERATIONS iterations | mode: ${RESULT_MODE} | priority: enabled | multirounds: ${MULTI_ROUNDS:-off}${NC}"
echo -e "${GREEN}========================================${NC}"

for vc in ${BENCH_VCS:-4 8 16 24 32}; do
    run_scenario wave $vc
    [ "$CLUSTER_MODE" = "allVehicles" ] && [ "$MULTI_ROUNDS" != "multirounds" ] && run_scenario udp $vc
    [ "$MULTI_ROUNDS" != "multirounds" ] && run_scenario wave $vc nopriority
done

echo -e "\n${GREEN}All runs complete. Plotting...${NC}\n"

cd "$SCRIPT_DIR"
echo -e "${GREEN}Running plot_comparison.py...${NC}"
python3 plot_comparison.py

echo -e "\n${GREEN}Done. Results in results/*_${RESULT_MODE}${NC}"
