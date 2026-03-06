#!/bin/bash
set -e
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NED_PATH="..:..:../../src:../../../inet/src:../../../inet/examples:../../../inet/tutorials:../../../inet/showcases:../../../../veins/examples/veins:../../../../veins/src/veins"
cd $SCRIPT_DIR/simulations/simple_intersection
$SCRIPT_DIR/src/benchmark -u Cmdenv -n "$NED_PATH" omnetpp_udp.ini --seed-set=1 > "$SCRIPT_DIR/full_sim.log" 2>&1
echo "Done"
