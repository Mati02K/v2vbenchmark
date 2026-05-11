# CLAUDE.md

V2V RAFT intersection coordination benchmark — OMNeT++ 5.6.2 · INET 4.4.x · Veins · SUMO.
See README.md for setup and installation.

---

## Build

OMNeT++ is not on PATH by default. Every build command needs:
```bash
export PATH="/home/mathesh/omnetpp-5.6.2/bin:$PATH"
export LD_LIBRARY_PATH="/home/mathesh/omnetpp-5.6.2/lib:$LD_LIBRARY_PATH"
cd src && make MODE=release all
```
Do not use `source setenv` — it requires a login shell and will fail.
Binary lands at `src/benchmark`.

**If build fails with "No rule to make target"**: the linter has reverted the Makefile. Re-apply:
```makefile
RAFT_DIR   = ../lib/raft
CRYPTO_DIR = ../lib/crypto
LIBS += -lssl -lcrypto
```

---

## Rules — read before every change

**Before writing or editing anything, apply both rule files:**
- `.claude/rules/design_rules.md` — naming, braces, comments, Python style
- `.claude/rules/workflow.md` — think first, simplicity, surgical edits, goal-driven execution

Key rules you must not miss:
- C++: camelCase + trailing `_`, `{}` on every `if`/`for`/`while` — no exceptions
- Python: snake_case, no bare `except: pass`, no magic numbers
- Both: no dead code, no comments that describe what the code does

---

## Architecture — edit the right file

All protocol logic lives in `RaftAppBase`. Two thin subclasses implement only 3 pure virtuals (`sendRaftUnicast`, `sendRaftBroadcast`, `getDistanceToJunction`). Never duplicate logic in subclasses.

| File | What goes here |
|---|---|
| `RaftAppBase.cc` | Constructor and `par()` reads only |
| `RaftCore.cc` | RAFT callbacks, `proposePassOrder`, QC signing |
| `RaftCoordination.cc` | Post-decision: batch advance, vehicle-left handling |
| `RaftDecision.cc` | `computePassOrder` scheduling algorithm |
| `RaftDiscovery.cc` | Beacon send/receive, cluster formation |
| `RaftUtilities.cc` | Shared helpers used by 2+ files |
| `RaftMetrics.cc` | JSON results file open/append/close |

---

## Timing Parameters

**All timing constants live in the `RaftAppBase` constructor. Never add them to ini or NED files.**

Check the constructor in `RaftAppBase.cc` for current values — they are tuned and change during experiments. The rule: if it's a timing value, it belongs in the constructor, not ini.

---

## Scenario Parameters (ini/NED only)

Only these belong in ini/NED:
`totalVehicles`, `intersectionStopDistance`, `clusterTriggerDistance`,
`discoveryBeaconInterval`, `approachEdges`, `exitEdges`,
`resultsFile`, `resultsFileCloseAtSec`, `isPriorityVehicle`, `allowMultipleRounds`

---

## SUMO

SUMO runs via `veins_launchd` on port 9999. The user always starts this — do not start it yourself. Before any simulation, verify:
```bash
ss -tlnp | grep 9999
```
If not listening, tell the user to start it and stop.

---

## Protocol Change Checklist

When editing protocol logic:
- [ ] `doApplyLog` handles both leader and follower uniformly
- [ ] QC signing path completes before `sendPassOrderBroadcast` is called
- [ ] `verifyQC()` is called in `handlePassOrderBroadcast` before trusting the schedule
- [ ] `passOrderProposed_` guard prevents double proposals
- [ ] `scheduleClusterFormBroadcastRetry` fires after `tryFormClusterFromCollected` to handle packet loss
- [ ] Queued (non-cluster) vehicles still handled via `PASS_ORDER_BROADCAST`
- [ ] Build passes with no new warnings
- [ ] Run QA check on 16-veh WAVE (most sensitive to regressions)

---

## QA Workflow

Always build before handing off to any agent:
```bash
cd src && make MODE=release all
```

| Goal | Skill |
|---|---|
| Validate a protocol change | `/test wave 16 allVehicles` |
| Run one specific combination | `/test wave 8 cluster 3` |
| Full benchmark, all scenarios | `/bench` |
| Full benchmark, specific mode | `/bench cluster 5` |
| Full benchmark, multirounds | `/bench multirounds 5` |

- **`/test`** — runs simulation in `/tmp/qatest_*/`, then automatically spawns `qaagent` for deep per-vehicle checks.
- **`/bench`** — runs all scenarios via `benchmark.sh`, replots graphs, then spawns `qaagent` on all result folders.

### Benchmark combinations (`benchmark.sh`)

Each mode runs two variants: priority rotation AND nopriority baseline (except multirounds — no nopriority).
WAVE runs for all modes; UDP runs only for `allVehicles`.

```bash
bash benchmark.sh <iters> cluster    # WAVE only: priority + nopriority
bash benchmark.sh <iters> allVehicles    # WAVE + UDP: priority + nopriority
bash benchmark.sh <iters> "" multirounds # WAVE only: priority only
```

Result directories per combination (× 6 vehicle counts: 4, 8, 16, 20, 24, 32):
- `simple_raftwave_<N>veh_cluster`
- `simple_raftwave_<N>veh_cluster_nopriority`
- `simple_raftwave_<N>veh_allVehicles`
- `simple_raftwave_<N>veh_allVehicles_nopriority`
- `simple_udp_<N>veh_allVehicles`
- `simple_raftwave_<N>veh_allVehicles_multirounds`

### Simulation directory

All vehicle counts share one directory: `simulations/simple_intersection`
The vehicle count is selected via `-c Veh4 / -c Veh8 / -c Veh16 / -c Veh20 / -c Veh24 / -c Veh32` config sections in the INI.

### Plot output

```bash
python3 plot_comparison.py   # per-transport PNGs (WAVE: all modes; UDP: allVehicles only)
python3 plot_heatmap.py      # channel_utilization_heatmap_wave.png + timeseries_wave.png
python3 plot_animation.py    # two GIFs: cluster + allvehicles (4→8→16→20→24→32)
```

`plot_comparison.py` generates PNG files — no arguments needed:
- `throughput_wave.png`, `throughput_udp.png`
- `leader_election_time_wave.png`, `leader_election_time_udp.png`
- `decision_time_wave.png`, `decision_time_udp.png`
- `fallbacks_wave.png`, `fallbacks_udp.png`
- `messages_wave.png`, `messages_udp.png`
- `cdf_wave.png`, `cdf_udp.png`
- `priority.png` — 3 bars: priority vehicle / normal vehicles / no-priority baseline

`plot_animation.py` generates two GIFs (priority dirs only):
- `results/channel_utilization_animation_cluster.gif`
- `results/channel_utilization_animation_allvehicles.gif`

Use `/results` to inspect existing data and `/plot` to regenerate graphs.
