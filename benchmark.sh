#!/bin/bash
#
# RAFT Intersection Benchmark Runner
# 
# Usage: ./benchmark.sh <vehicle_count> <result_name>
# Example: ./benchmark.sh 4 four_vehicles
#          ./benchmark.sh 8 eight_vehicles
#
# Results will be saved in: benchmark/results/<result_name>/
#

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Paths
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SIM_DIR="$SCRIPT_DIR/simulations/raft"
SRC_DIR="$SCRIPT_DIR/src"
RESULTS_BASE="$SCRIPT_DIR/results"

# NED path for OMNeT++
NED_PATH="..:..:../../src:../../../inet/src:../../../inet/examples:../../../inet/tutorials:../../../inet/showcases:../../../../veins/examples/veins:../../../../veins/src/veins"

# Check arguments
if [ $# -lt 2 ]; then
    echo -e "${RED}Usage: $0 <vehicle_count> <result_name>${NC}"
    echo ""
    echo "Examples:"
    echo "  $0 4 test_4veh"
    echo "  $0 8 test_8veh"
    echo "  $0 16 large_test"
    echo ""
    echo "Supported vehicle counts: 3, 4, 8, 16, 32"
    exit 1
fi

VEHICLE_COUNT=$1
RESULT_NAME=$2

# Validate vehicle count
if [[ ! "$VEHICLE_COUNT" =~ ^(3|4|8|16|32)$ ]]; then
    echo -e "${RED}Error: Vehicle count must be 3, 4, 8, 16, or 32${NC}"
    exit 1
fi

echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}  RAFT Intersection Benchmark${NC}"
echo -e "${GREEN}========================================${NC}"
echo ""
echo -e "Vehicles:    ${YELLOW}$VEHICLE_COUNT${NC}"
echo -e "Result Name: ${YELLOW}$RESULT_NAME${NC}"
echo ""

# Create results directory
RESULT_DIR="$RESULTS_BASE/$RESULT_NAME"
mkdir -p "$RESULT_DIR"
echo -e "Results dir: ${YELLOW}$RESULT_DIR${NC}"
echo ""

# Generate route file if needed
ROUTE_FILE="$SIM_DIR/intersection_${VEHICLE_COUNT}veh.rou.xml"
if [ ! -f "$ROUTE_FILE" ]; then
    echo -e "${YELLOW}Generating route file for $VEHICLE_COUNT vehicles...${NC}"
    cd "$SIM_DIR"
    python3 generate_config.py "$VEHICLE_COUNT"
fi

# Copy route file to active location
echo -e "Setting up route file..."
cp "$ROUTE_FILE" "$SIM_DIR/intersection.rou.xml"

# Create temporary ini file with correct output path
TEMP_INI="$SIM_DIR/temp_benchmark.ini"
RESULTS_JSON="$RESULT_DIR/raft_results.json"

cat > "$TEMP_INI" << EOF
[General]
network = IntersectionScenario
sim-time-limit = 120s
**.cmdenv-log-level = warn

# Manager
*.manager.moduleType = "benchmark.veins_inet.VeinsInetCar"
*.manager.moduleName = "node"
*.manager.moduleDisplayString = ""
*.manager.launchConfig = xmldoc("intersection.launchd.xml")
*.manager.autoShutdown = true
*.manager.updateInterval = 0.1s
*.manager.host = "localhost"
*.manager.port = 9999

# Scenario
*.node[*].mobility.typename = "VeinsInetMobility"

# Application
*.node[*].numApps = 1
*.node[*].app[0].typename = "WillemtRaftApplication"
*.node[*].app[0].totalVehicles = $VEHICLE_COUNT
*.node[*].app[0].resultsFile = "$RESULTS_JSON"
*.node[*].app[0].interface = "wlan0"
*.node[*].app[0].destPort = 9001
*.node[*].app[0].localPort = 9001
*.node[*].app[0].destAddress = "255.255.255.255"

# Timing - scale with vehicle count
*.node[*].app[0].electionTimeoutBaseMs = 150
*.node[*].app[0].electionTimeoutJitterMs = $((50 / (VEHICLE_COUNT / 4 + 1)))
*.node[*].app[0].requestTimeoutMs = 50
*.node[*].app[0].maxFailedElections = $((VEHICLE_COUNT / 2 + 4))
*.node[*].app[0].fallbackWaitMinMs = 100
*.node[*].app[0].fallbackWaitMaxMs = 300
*.node[*].app[0].passConfirmationMs = 300
*.node[*].app[0].statusCollectionTimeoutMs = 150

# Radio
*.node[*].wlan[0].radio.transmitter.power = 20mW
*.node[*].wlan[0].typename = "Ieee80211Interface"
*.node[*].wlan[0].radio.typename = "Ieee80211ScalarRadio"
*.node[*].wlan[0].mac.dcf.channelAccess.cwMin = 7
*.node[*].wlan[0].radio.transmitter.communicationRange = 500m
*.node[*].wlan[*].radio.receiver.sensitivity = -89dBm
*.node[*].wlan[*].radio.receiver.snirThreshold = 4dB
EOF

# Run simulation
echo -e "${GREEN}Running simulation...${NC}"
echo ""

cd "$SIM_DIR"
LOG_FILE="$RESULT_DIR/console.log"

# Run and capture output
"$SRC_DIR/benchmark_dbg" -u Cmdenv -n "$NED_PATH" "$TEMP_INI" 2>&1 | tee "$LOG_FILE"

# Check if results were generated
if [ ! -f "$RESULTS_JSON" ]; then
    echo -e "${RED}Error: Results file not generated!${NC}"
    rm -f "$TEMP_INI"
    exit 1
fi

echo ""
echo -e "${GREEN}Simulation complete!${NC}"
echo ""

# Generate plots
echo -e "${YELLOW}Generating plots...${NC}"
cd "$SIM_DIR"
python3 - "$RESULTS_JSON" "$RESULT_DIR" << 'PYTHON_SCRIPT'
import json
import sys
import os
import matplotlib.pyplot as plt
import numpy as np

def load_results(filename):
    with open(filename, 'r') as f:
        return json.load(f)

def calculate_summary(results):
    raft_vehicles = [v for v in results if v['coordination_method'] == 'raft']
    fallback_vehicles = [v for v in results if v['coordination_method'] == 'fallback']
    
    total_count = len(results)
    raft_count = len(raft_vehicles)
    
    if raft_vehicles:
        raft_wait_times = [v['durations_ms']['total_wait_time'] for v in raft_vehicles]
        raft_election_times = [v['durations_ms']['election_time'] for v in raft_vehicles]
        raft_first_stop = min([v['timestamps_ms']['stopped'] for v in raft_vehicles])
        raft_last_pass = max([v['timestamps_ms']['passed'] for v in raft_vehicles])
        total_raft_time_ms = raft_last_pass - raft_first_stop
    else:
        raft_wait_times = []
        raft_election_times = []
        total_raft_time_ms = 0
    
    all_first_stop = min([v['timestamps_ms']['stopped'] for v in results])
    all_last_pass = max([v['timestamps_ms']['passed'] for v in results])
    total_time_ms = all_last_pass - all_first_stop
    
    return {
        'total_vehicles': total_count,
        'raft_count': raft_count,
        'fallback_count': len(fallback_vehicles),
        'raft_avg_wait_ms': np.mean(raft_wait_times) if raft_wait_times else 0,
        'raft_avg_election_ms': np.mean(raft_election_times) if raft_election_times else 0,
        'total_raft_time_ms': total_raft_time_ms,
        'total_time_ms': total_time_ms,
    }

def plot_timeline(results, output_dir):
    fig, ax = plt.subplots(figsize=(14, max(6, len(results) * 0.4)))
    
    colors = {'raft': '#2ecc71', 'fallback': '#e74c3c'}
    
    for v in results:
        vid = v['vehicle_id']
        method = v['coordination_method']
        ts = v['timestamps_ms']
        
        stopped = ts['stopped']
        passed = ts['passed']
        
        ax.barh(vid, passed - stopped, left=stopped, 
                color=colors[method], alpha=0.7, height=0.6)
        
        if v['was_leader'] and ts['elected'] > 0:
            ax.scatter([ts['elected']], [vid], color='blue', s=100, zorder=5, marker='*')
    
    # Legend
    from matplotlib.patches import Patch
    legend_elements = [
        Patch(facecolor='#2ecc71', alpha=0.7, label='RAFT'),
        Patch(facecolor='#e74c3c', alpha=0.7, label='Fallback'),
        plt.Line2D([0], [0], marker='*', color='w', markerfacecolor='blue', markersize=10, label='Elected Leader')
    ]
    ax.legend(handles=legend_elements, loc='upper right')
    
    ax.set_xlabel('Time (ms)', fontsize=12)
    ax.set_ylabel('Vehicle ID', fontsize=12)
    ax.set_title(f'RAFT Intersection Timeline ({len(results)} vehicles)', fontsize=14)
    ax.set_yticks(range(len(results)))
    ax.grid(True, alpha=0.3, axis='x')
    
    plt.tight_layout()
    plt.savefig(os.path.join(output_dir, 'timeline.png'), dpi=150)
    plt.close()

def plot_metrics(results, output_dir):
    fig, axes = plt.subplots(2, 2, figsize=(12, 10))
    
    vehicle_ids = [v['vehicle_id'] for v in results]
    wait_times = [v['durations_ms']['total_wait_time'] for v in results]
    methods = [v['coordination_method'] for v in results]
    msgs_sent = [v['messages']['sent'] for v in results]
    msgs_recv = [v['messages']['received'] for v in results]
    
    colors = ['#2ecc71' if m == 'raft' else '#e74c3c' for m in methods]
    
    # 1. Wait Time
    ax1 = axes[0, 0]
    ax1.bar(vehicle_ids, wait_times, color=colors, alpha=0.7)
    ax1.set_xlabel('Vehicle ID')
    ax1.set_ylabel('Wait Time (ms)')
    ax1.set_title('Total Wait Time per Vehicle')
    
    # 2. Messages
    ax2 = axes[0, 1]
    x = np.arange(len(vehicle_ids))
    width = 0.35
    ax2.bar(x - width/2, msgs_sent, width, label='Sent', color='#3498db', alpha=0.7)
    ax2.bar(x + width/2, msgs_recv, width, label='Received', color='#9b59b6', alpha=0.7)
    ax2.set_xlabel('Vehicle ID')
    ax2.set_ylabel('Message Count')
    ax2.set_title('Messages Exchanged')
    ax2.set_xticks(x)
    ax2.set_xticklabels(vehicle_ids)
    ax2.legend()
    
    # 3. Coordination breakdown
    ax3 = axes[1, 0]
    raft_count = sum(1 for m in methods if m == 'raft')
    fallback_count = len(methods) - raft_count
    ax3.pie([raft_count, fallback_count], labels=['RAFT', 'Fallback'], 
            colors=['#2ecc71', '#e74c3c'], autopct='%1.0f%%', startangle=90)
    ax3.set_title('Coordination Method Distribution')
    
    # 4. Summary
    ax4 = axes[1, 1]
    ax4.axis('off')
    
    summary = calculate_summary(results)
    
    text = f"""
    SUMMARY
    ═══════════════════════════════
    
    Total Vehicles:     {summary['total_vehicles']}
    RAFT Elected:       {summary['raft_count']}
    Fallback:           {summary['fallback_count']}
    
    Avg RAFT Election:  {summary['raft_avg_election_ms']:.1f} ms
    Avg RAFT Wait:      {summary['raft_avg_wait_ms']:.1f} ms
    
    Total RAFT Time:    {summary['total_raft_time_ms']:.1f} ms
    Total Time:         {summary['total_time_ms']:.1f} ms
    
    Total Msgs Sent:    {sum(msgs_sent)}
    Total Msgs Recv:    {sum(msgs_recv)}
    """
    
    ax4.text(0.1, 0.9, text, transform=ax4.transAxes, fontsize=11,
             verticalalignment='top', fontfamily='monospace',
             bbox=dict(boxstyle='round', facecolor='wheat', alpha=0.5))
    
    plt.tight_layout()
    plt.savefig(os.path.join(output_dir, 'metrics.png'), dpi=150)
    plt.close()

def write_summary(results, output_dir):
    summary = calculate_summary(results)
    
    with open(os.path.join(output_dir, 'summary.txt'), 'w') as f:
        f.write("RAFT INTERSECTION BENCHMARK RESULTS\n")
        f.write("="*50 + "\n\n")
        f.write(f"Total Vehicles:     {summary['total_vehicles']}\n")
        f.write(f"RAFT Elected:       {summary['raft_count']}\n")
        f.write(f"Fallback:           {summary['fallback_count']}\n\n")
        f.write("--- RAFT-Only Metrics ---\n")
        f.write(f"Avg Election Time:  {summary['raft_avg_election_ms']:.1f} ms\n")
        f.write(f"Avg Wait Time:      {summary['raft_avg_wait_ms']:.1f} ms\n")
        f.write(f"Total RAFT Time:    {summary['total_raft_time_ms']:.1f} ms\n\n")
        f.write("--- Overall ---\n")
        f.write(f"Total Time:         {summary['total_time_ms']:.1f} ms\n")
        f.write("="*50 + "\n")

if __name__ == "__main__":
    results_file = sys.argv[1]
    output_dir = sys.argv[2]
    
    results = load_results(results_file)
    
    plot_timeline(results, output_dir)
    plot_metrics(results, output_dir)
    write_summary(results, output_dir)
    
    print(f"Plots saved to {output_dir}")
PYTHON_SCRIPT

# Cleanup
rm -f "$TEMP_INI"

# Print summary
echo ""
echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}  Benchmark Complete!${NC}"
echo -e "${GREEN}========================================${NC}"
echo ""
echo -e "Results saved to: ${YELLOW}$RESULT_DIR/${NC}"
echo ""
echo "Files:"
ls -la "$RESULT_DIR/"
echo ""
echo -e "${GREEN}Done!${NC}"
