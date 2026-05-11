Run the full benchmark for all scenarios, QA all result logs, then replot graphs.

Arguments: $ARGUMENTS

## Argument Parsing

| Arg | Values | Default |
|---|---|---|
| iterations | any integer | `10` |
| mode | `cluster`, `allVehicles`, `multirounds`, or `all` | `all` |

Each mode runs priority rotation AND nopriority baseline (except multirounds — priority only).
WAVE runs for all modes. UDP runs only for `allVehicles`.

Examples:
- (empty) → 10 iters, all 3 combinations
- `5` → 5 iters, all 3 combinations
- `cluster 3` → 3 iters, cluster only (WAVE, priority + nopriority)
- `allVehicles 5` → 5 iters, allVehicles (WAVE + UDP, priority + nopriority)
- `multirounds 5` → 5 iters, multirounds (WAVE only, priority only)

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

bash benchmark.sh <iterations> cluster    # WAVE only
bash benchmark.sh <iterations> allVehicles    # WAVE + UDP
bash benchmark.sh <iterations> "" multirounds # WAVE only
```

Only run the combinations matching the parsed arguments. Report each script's exit status.

Result directories per combination (× 6 vehicle counts: 4, 8, 16, 20, 24, 32):
- `cluster` → `simple_raftwave_<N>veh_cluster`, `simple_raftwave_<N>veh_cluster_nopriority`
- `allVehicles` → `simple_raftwave_<N>veh_allVehicles`, `simple_raftwave_<N>veh_allVehicles_nopriority`, `simple_udp_<N>veh_allVehicles`
- `multirounds` → `simple_raftwave_<N>veh_allVehicles_multirounds`

## Step 3 — Replot graphs

After all scripts complete, run all plot scripts:

```bash
cd /home/mathesh/omnetpp-workspace/benchmark
python3 plot_comparison.py
python3 plot_heatmap.py
python3 plot_animation.py
```

`plot_comparison.py` generates PNG files:
- `throughput_wave.png`, `throughput_udp.png`
- `leader_election_time_wave.png`, `leader_election_time_udp.png`
- `decision_time_wave.png`, `decision_time_udp.png`
- `fallbacks_wave.png`, `fallbacks_udp.png`
- `messages_wave.png`, `messages_udp.png`
- `cdf_wave.png`, `cdf_udp.png`
- `priority.png` — 3 bars: priority vehicle / normal vehicles / no-priority baseline

`plot_heatmap.py` generates:
- `channel_utilization_heatmap_wave.png`
- `channel_utilization_timeseries_wave.png`

`plot_animation.py` generates (priority dirs only):
- `channel_utilization_animation_cluster.gif`
- `channel_utilization_animation_allvehicles.gif`

Report which files were saved.

## Step 4 — QA

Spawn a `qaagent` and pass it the list of result folders that were just populated.

For `cluster`:
- `results/simple_raftwave_4veh_cluster/`
- `results/simple_raftwave_8veh_cluster/`
- `results/simple_raftwave_16veh_cluster/`
- `results/simple_raftwave_20veh_cluster/`
- `results/simple_raftwave_24veh_cluster/`
- `results/simple_raftwave_32veh_cluster/`

For `allVehicles`:
- `results/simple_raftwave_4veh_allVehicles/`
- `results/simple_raftwave_8veh_allVehicles/`
- `results/simple_raftwave_16veh_allVehicles/`
- `results/simple_raftwave_20veh_allVehicles/`
- `results/simple_raftwave_24veh_allVehicles/`
- `results/simple_raftwave_32veh_allVehicles/`
- `results/simple_udp_4veh_allVehicles/`
- `results/simple_udp_8veh_allVehicles/`
- `results/simple_udp_16veh_allVehicles/`
- `results/simple_udp_20veh_allVehicles/`
- `results/simple_udp_24veh_allVehicles/`
- `results/simple_udp_32veh_allVehicles/`

For `multirounds`:
- `results/simple_raftwave_4veh_allVehicles_multirounds/`
- `results/simple_raftwave_8veh_allVehicles_multirounds/`
- `results/simple_raftwave_16veh_allVehicles_multirounds/`
- `results/simple_raftwave_20veh_allVehicles_multirounds/`
- `results/simple_raftwave_24veh_allVehicles_multirounds/`
- `results/simple_raftwave_32veh_allVehicles_multirounds/`

### QA thresholds per combination type

**Standard combinations** (`cluster`, `allVehicles`):
- `coordination_method != "raft"` → FAIL (fallbacks are never acceptable)
- `log_entries_committed == 0` for a vehicle in `allVehicles` mode → WARN (may have received schedule via broadcast gossip — functionally correct, not a FAIL)
- `log_entries_committed == 0` for non-lane-leader in `cluster` mode → expected
- `election_rounds > 10` → WARN

**Multirounds combinations** (`_multirounds` suffix):
- Fallbacks are **expected and acceptable**. After round 1 the remaining cluster shrinks — do NOT flag fallbacks as failures.
- `my_batch=-1` → expected and acceptable for multirounds.
- `coordination_method="fallback"` → expected and acceptable for multirounds.
- `log_entries_committed >= 1` for any vehicle that participated is sufficient.
- `election_rounds > 20` → WARN only (spikes of 50–100 are normal at 16+ vehicles).
- Report fallback rate as informational only.
