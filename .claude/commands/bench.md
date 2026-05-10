Run the full benchmark for all scenarios, QA all result logs, then replot graphs.

Arguments: $ARGUMENTS

## Argument Parsing

| Arg | Values | Default |
|---|---|---|
| iterations | any integer | `10` |
| mode | `laneLeaders`, `allVehicles`, `multirounds`, or `both` | `both` |
| priority | `nopriority` to run nopriority only | both priority AND nopriority run by default |

Examples:
- (empty) → 10 iters, all 4 standard combinations (laneLeaders, allVehicles, laneLeaders_nopriority, allVehicles_nopriority)
- `5` → 5 iters, all 4 standard combinations
- `laneLeaders 3` → 3 iters, laneLeaders + laneLeaders_nopriority
- `allVehicles nopriority` → 10 iters, allVehicles nopriority only
- `multirounds 5` → 5 iters, multirounds + priority AND multirounds + nopriority

## Step 1 — Verify prerequisites

```bash
ls /home/mathesh/omnetpp-workspace/benchmark/src/benchmark
ss -tlnp | grep 9999
```

If binary missing: stop and say "Build first". If SUMO not on 9999: stop and say "Start veins_launchd first".

## Step 2 — Run benchmark scripts

Run each combination sequentially (SUMO port 9999 allows only one at a time):

```bash
cd /home/mathesh/omnetpp-workspace/benchmark
export PATH="/home/mathesh/omnetpp-5.6.2/bin:$PATH"
export LD_LIBRARY_PATH="/home/mathesh/omnetpp-5.6.2/lib:$LD_LIBRARY_PATH"

# Standard combinations
bash benchmark.sh <iterations> laneLeaders                    # priority + laneLeaders
bash benchmark.sh <iterations> allVehicles                    # priority + allVehicles
bash benchmark.sh <iterations> laneLeaders nopriority         # nopriority + laneLeaders
bash benchmark.sh <iterations> allVehicles nopriority         # nopriority + allVehicles

# Multirounds combinations (forces allVehicles internally)
bash benchmark.sh <iterations> "" "" multirounds              # multirounds + priority
bash benchmark.sh <iterations> "" nopriority multirounds      # multirounds + nopriority
```

Only run the combinations matching the parsed arguments. Report each script's exit status.

Result directory suffix per combination:
- `laneLeaders`, `laneLeaders_nopriority`
- `allVehicles`, `allVehicles_nopriority`
- `allVehicles_multirounds`, `allVehicles_nopriority_multirounds`

## Step 3 — Replot graphs

After all scripts complete, run once (no arguments needed — scans all result dirs):

```bash
cd /home/mathesh/omnetpp-workspace/benchmark
python3 plot_comparison.py
```

This generates 13 PNGs:
- `throughput_wave.png`, `throughput_udp.png`
- `leader_election_time_wave.png`, `leader_election_time_udp.png`
- `decision_time_wave.png`, `decision_time_udp.png`
- `fallbacks_wave.png`, `fallbacks_udp.png`
- `messages_wave.png`, `messages_udp.png`
- `cdf_wave.png`, `cdf_udp.png`
- `priority.png`

Report which PNGs were saved.

## Step 4 — QA

Spawn a `qaagent` and pass it the list of result folders that were just populated:
- `results/simple_udp_4veh_<result_mode>/`
- `results/simple_udp_8veh_<result_mode>/`
- `results/simple_udp_16veh_<result_mode>/`
- `results/simple_raftwave_4veh_<result_mode>/`
- `results/simple_raftwave_8veh_<result_mode>/`
- `results/simple_raftwave_16veh_<result_mode>/`

(one set per combination that was run; `<result_mode>` matches the directory suffix above)

### QA thresholds per combination type

**Standard combinations** (`laneLeaders`, `allVehicles`, and their `_nopriority` variants):
- `coordination_method != "raft"` → FAIL (fallbacks are never acceptable)
- `log_entries_committed == 0` for a vehicle in `allVehicles` mode → WARN (vehicle may have received schedule via broadcast gossip instead of RAFT replication — functionally correct, not a FAIL)
- `log_entries_committed == 0` for non-lane-leader in `laneLeaders` mode → expected (they don't join RAFT)
- `election_rounds > 10` → WARN

**Multirounds combinations** (`_multirounds` suffix):
- Fallbacks are **expected and acceptable**. After round 1, the remaining cluster shrinks. With only 2 vehicles left, RAFT quorum is hard to achieve (especially over UDP with packet loss) — these vehicles fall back to the timer-based mechanism. Do NOT flag fallbacks as failures for multirounds.
- `my_batch=-1` means the vehicle is in fallback mode. This is **expected and acceptable** for multirounds — do NOT flag it as a FAIL.
- `coordination_method="fallback"` is **expected and acceptable** for multirounds — do NOT flag it as a FAIL.
- `log_entries_committed >= 1` for any vehicle that did participate is sufficient — do NOT require 2+ commits (a vehicle scheduled in round 1 only ever sees 1 committed entry).
- `election_rounds > 20` → WARN only, never FAIL (higher threshold because round 2 restarts election; spikes of 50–100 are seen at 16 vehicles and are expected).
- Report fallback rate as informational only (expected ~10–20% for 8-veh, higher for 16-veh).
