#!/bin/bash
#
# Run UDP/INET benchmark with simple 4-way intersection (no Santa Cruz map).
# 4 straight approaches, 16 vehicles pre-spaced near the center.
# Uses 802.11a Wi-Fi (UDP) instead of WAVE/802.11p.
#
# Prerequisite: veins_launchd must be running (SUMO will be started by it)
#   python3 /path/to/veins/bin/veins_launchd -vv -c sumo-gui
#
# Usage: ./run_simple_udp.sh

set -e
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SIM_DIR="$SCRIPT_DIR/simulations/simple_intersection"
SRC_DIR="$SCRIPT_DIR/src"

NED_PATH="..:..:../../src:../../../inet/src:../../../inet/examples:../../../inet/tutorials:../../../inet/showcases:../../../../veins/examples/veins:../../../../veins/src/veins"

if [ ! -d "$SIM_DIR" ]; then
    echo "Error: $SIM_DIR not found"
    exit 1
fi

if [ ! -x "$SRC_DIR/benchmark" ]; then
    echo "Error: Build benchmark first: cd src && make"
    exit 1
fi

echo "Running simple intersection UDP (16 vehicles, 4 straight edges)..."
cd "$SIM_DIR"
"$SRC_DIR/benchmark" -u Cmdenv -n "$NED_PATH" omnetpp_udp.ini 2>&1
