#!/bin/bash
# Simple intersection benchmark: UDP + WAVE for 4, 8, 16 vehicles.
#
# Usage: ./benchmark.sh [iterations] [cluster_mode] [nopriority] [multirounds]
#
#   iterations   : number of runs per scenario (default: 3)
#   cluster_mode : laneLeaders (default) | allVehicles
#                  NOTE: ignored when multirounds is set — forced to allVehicles
#   nopriority   : pass "nopriority" to disable priority vehicle rotation
#                  omit (or pass "") to rotate a priority vehicle each run
#   multirounds  : pass "multirounds" to allow extra RAFT rounds even when all
#                  vehicles are already scheduled; forces cluster_mode=allVehicles
#
# Valid combinations:
#   ./benchmark.sh 3 laneLeaders                    # laneLeaders + priority
#   ./benchmark.sh 3 laneLeaders nopriority         # laneLeaders + no priority
#   ./benchmark.sh 3 allVehicles                    # allVehicles + priority
#   ./benchmark.sh 3 allVehicles nopriority         # allVehicles + no priority
#   ./benchmark.sh 3 "" "" multirounds              # multirounds + priority
#   ./benchmark.sh 3 "" nopriority multirounds      # multirounds + no priority

set -e

GREEN='\033[0;32m'; RED='\033[0;31m'; NC='\033[0m'

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_DIR="$SCRIPT_DIR/src"
RESULTS_BASE="$SCRIPT_DIR/results"
NED_PATH="..:..:../../src:../../../inet/src:../../../inet/examples:../../../inet/tutorials:../../../inet/showcases:../../../../veins/examples/veins:../../../../veins/src/veins"

NUM_ITERATIONS=${1:-3}
CLUSTER_MODE=${2:-laneLeaders}
NO_PRIORITY=${3:-}   # set to "nopriority" to disable rotating priority vehicle
MULTI_ROUNDS=${4:-}  # set to "multirounds" to enable allowMultipleRounds

# multirounds requires allVehicles so all present vehicles can participate in round 2+
if [ "$MULTI_ROUNDS" = "multirounds" ]; then
    [ -n "${2:-}" ] && [ "${2:-}" != "allVehicles" ] && \
        echo -e "${RED}Warning: cluster_mode='${2}' ignored — multirounds forces allVehicles${NC}"
    CLUSTER_MODE="allVehicles"
fi

# Result directories get suffixes for each active option so runs never overwrite each other.
RESULT_MODE="${CLUSTER_MODE}$( [ "$NO_PRIORITY" = "nopriority" ] && echo "_nopriority" || echo "" )$( [ "$MULTI_ROUNDS" = "multirounds" ] && echo "_multirounds" || echo "" )"

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
        local allow_multi=$( [ "$MULTI_ROUNDS" = "multirounds" ] && echo "true" || echo "false" )
        "$SRC_DIR/benchmark" -u Cmdenv -n "$NED_PATH" "$ini" -f "$prio_ini" \
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
for vc in 4 8 16; do
    [ -d "$(get_sim_dir $vc)" ] || { echo -e "${RED}Error: sim dir for ${vc}veh not found${NC}"; exit 1; }
done

echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}  Simple Intersection Benchmark${NC}"
echo -e "${GREEN}  UDP + WAVE for 4, 8, 16 vehicles${NC}"
echo -e "${GREEN}  $NUM_ITERATIONS iterations | mode: ${RESULT_MODE} | priority: ${NO_PRIORITY:-enabled} | multirounds: ${MULTI_ROUNDS:-off}${NC}"
echo -e "${GREEN}========================================${NC}"

for vc in 4 8 16; do
    run_scenario udp  $vc
    run_scenario wave $vc
done

echo -e "\n${GREEN}All runs complete. Plotting...${NC}\n"

cd "$SCRIPT_DIR"
echo -e "${GREEN}Running plot_comparison.py...${NC}"
python3 plot_comparison.py

echo -e "\n${GREEN}Done. Results in results/*_${RESULT_MODE}${NC}"
