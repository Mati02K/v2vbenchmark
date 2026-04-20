Run an isolated simulation combination and report the output paths.

Arguments: $ARGUMENTS

## Argument Parsing

Parse `$ARGUMENTS` to determine:

| Arg | Values | Default |
|---|---|---|
| transport | `udp` or `wave` | `wave` |
| vehicles | `4`, `8`, or `16` | `4` |
| iterations | any integer not in {4,8,16} | `1` |
| mode | `laneLeaders` or `allVehicles` | `laneLeaders` |
| priority | `nopriority` to disable | priority enabled (rotates V0..VN-1) |

Examples:
- (empty) → wave, 4 veh, 1 iter, laneLeaders, priority
- `wave 16 allVehicles` → wave, 16 veh, 1 iter, allVehicles, priority
- `wave 16 allVehicles 10` → wave, 16 veh, 10 iters, allVehicles, priority
- `udp 8 3 nopriority` → udp, 8 veh, 3 iters, laneLeaders, nopriority
- `udp 16 allVehicles 5` → udp, 16 veh, 5 iters, allVehicles, priority

## Isolation

Each combination writes to its own directory so concurrent runs never collide:

```
/tmp/qatest_<transport>_<vehicles>veh_<mode>/run_1/
/tmp/qatest_<transport>_<vehicles>veh_<mode>/run_2/
...
```

Always wipe and recreate the base dir before starting so stale data from a previous test run is never mixed in:
```bash
rm -rf "/tmp/qatest_<transport>_<vehicles>veh_<mode>"
mkdir -p "/tmp/qatest_<transport>_<vehicles>veh_<mode>"
```

## Environment

```bash
export PATH="/home/mathesh/omnetpp-5.6.2/bin:$PATH"
export LD_LIBRARY_PATH="/home/mathesh/omnetpp-5.6.2/lib:$LD_LIBRARY_PATH"

SCRIPT_DIR=/home/mathesh/omnetpp-workspace/benchmark
SRC_DIR=$SCRIPT_DIR/src
NED_PATH="..:..:../../src:../../../inet/src:../../../inet/examples:../../../inet/tutorials:../../../inet/showcases:../../../../veins/examples/veins:../../../../veins/src/veins"
```

## Simulation directory

| Vehicles | Directory |
|---|---|
| 4  | `$SCRIPT_DIR/simulations/simple_intersection_4` |
| 8  | `$SCRIPT_DIR/simulations/simple_intersection_8` |
| 16 | `$SCRIPT_DIR/simulations/simple_intersection` |

## INI file and app key

| Transport | INI file | app key |
|---|---|---|
| udp  | `omnetpp_udp.ini`  | `app[0]` |
| wave | `omnetpp_wave.ini` | `appl`   |

## Run loop

For each iteration i from 1 to <iterations>:

```bash
RESULT_DIR="/tmp/qatest_<transport>_<vehicles>veh_<mode>"
ITER_DIR="$RESULT_DIR/run_$i"
RESULTS_JSON="$ITER_DIR/raft_results.json"
CONSOLE_LOG="$ITER_DIR/console.log"
mkdir -p "$ITER_DIR"

# Priority ini — rotates vehicle each run: vid = (i-1) % vehicles
PRIO_INI="/tmp/qatest_prio_$$.ini"
if nopriority:
    printf '[General]\n**.isPriorityVehicle = false\n' > "$PRIO_INI"
else:
    vid=$(( (i - 1) % <vehicles> ))
    printf '[General]\n**.node[%d].<app_key>.isPriorityVehicle = true\n**.isPriorityVehicle = false\n' \
        "$vid" > "$PRIO_INI"

cd <sim_dir>
"$SRC_DIR/benchmark" -u Cmdenv -n "$NED_PATH" <ini_file> -f "$PRIO_INI" \
    --seed-set=$i \
    "--**.<app_key>.resultsFile=\"$RESULTS_JSON\"" \
    "--**.<app_key>.clusterMode=\"<mode>\"" \
    > "$CONSOLE_LOG" 2>&1
rm -f "$PRIO_INI"

echo "  Run $i done"
```

## Output

After all iterations print:

```
RESULT_DIR: /tmp/qatest_<transport>_<vehicles>veh_<mode>
ITERATIONS: <N>
```

Then for each run print:
```
RUN 1: RESULTS_JSON=/tmp/.../run_1/raft_results.json  CONSOLE_LOG=/tmp/.../run_1/console.log  STATUS=OK|FAIL
RUN 2: ...
```

Status per run:
- `OK` — binary exited 0 and JSON file exists and is non-empty
- `FAIL (no JSON)` — JSON missing
- `FAIL (exit <code>)` — binary returned non-zero

## QA

After all runs complete, spawn `qaagent` and pass it the result directory:
`/tmp/qatest_<transport>_<vehicles>veh_<mode>/`

Report the qaagent output in full.
