#!/bin/bash
# Runs all benchmark combinations sequentially, then regenerates all plots.
#
# Usage: ./run_all.sh [iterations]
#   iterations : runs per scenario (default: 10)

set -e

GREEN='\033[0;32m'; RED='\033[0;31m'; NC='\033[0m'

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ITERS=${1:-10}
export BENCH_VCS="4 8 16"

export PATH="/home/mathesh/omnetpp-5.6.2/bin:$PATH"
export LD_LIBRARY_PATH="/home/mathesh/omnetpp-5.6.2/lib:$LD_LIBRARY_PATH"

# ── Prerequisites ─────────────────────────────────────────────────────────────
[ -x "$SCRIPT_DIR/src/benchmark" ] || { echo -e "${RED}Error: build first: cd src && make${NC}"; exit 1; }
ss -tlnp | grep -q 9999 || { echo -e "${RED}Error: SUMO not running on port 9999 — start veins_launchd first${NC}"; exit 1; }

echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}  Full Benchmark — $ITERS iterations${NC}"
echo -e "${GREEN}  laneLeaders → allVehicles → multirounds${NC}"
echo -e "${GREEN}========================================${NC}"

cd "$SCRIPT_DIR"

echo -e "\n${GREEN}[1/3] laneLeaders (WAVE)${NC}"
bash benchmark.sh "$ITERS" laneLeaders

echo -e "\n${GREEN}[2/3] allVehicles (WAVE + UDP)${NC}"
bash benchmark.sh "$ITERS" allVehicles

echo -e "\n${GREEN}[3/3] multirounds (WAVE)${NC}"
bash benchmark.sh "$ITERS" "" multirounds

echo -e "\n${GREEN}========================================${NC}"
echo -e "${GREEN}  All runs complete. Plotting...${NC}"
echo -e "${GREEN}========================================${NC}"

python3 plot_comparison.py
python3 plot_heatmap.py
python3 plot_animation.py

echo -e "\n${GREEN}Done. Results in results/${NC}"
