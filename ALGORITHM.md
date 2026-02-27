# V2V RAFT Intersection Coordination — Algorithm Reference

> Stack: **OMNeT++ 5.6.2 + INET 4.4.x + Veins + SUMO**
> Protocols compared: **UDP over IEEE 802.11a** vs **WAVE over IEEE 802.11p**
> Benchmark: **4, 8, 16 vehicles** | Stop distance **5 m** | Cluster wait **7 s** (5 s discovery + 2 s delay)

---

## Current Algorithm Summary (Simple Intersection)

| Parameter | Value |
|-----------|-------|
| Vehicle counts | 4, 8, 16 |
| `intersectionStopDistance` | 5 m |
| `discoveryWaitMs` | 5000 ms |
| `clusterFormationDelayMs` | 2000 ms |
| UDP channel | CH36 (5.18 GHz 802.11a) |
| WAVE channel | CH178 (5.89 GHz 802.11p CCH) |
| Cluster formation | 7 s after first vehicle stops |
| Throughput formula | N_raft / (last_raft_passed − first_raft_start) |
| UDP receiver sensitivity | -82 dBm (802.11a 20 MHz @ 6 Mbps) |
| WAVE sensitivity | -88 dBm (802.11p 10 MHz @ 6 Mbps) |
| Transmit power (both) | 200 mW (23 dBm, OBU-class) |
| Merge rule | Incoming cluster must be strictly larger |

---

## 0. What Each File Does

| File | Role |
|------|------|
| `src/raft/RaftAppBase.h / .cc` | **Abstract base class (~1400 lines).** Contains 100% of the shared RAFT protocol logic — cluster formation, leader election callbacks, status collection, pass-order generation, batch execution, intersection detection, fallback handling, and JSON metrics. Neither UDP nor WAVE logic lives here. |
| `src/raft/WillemtRaftApplication.h / .cc` | **UDP subclass (~450 lines).** Extends `VeinsInetApplicationBase` (INET). Implements the 6 pure-virtual transport methods using INET `UdpSocket` on 802.11a. Handles OMNeT++ packet send/receive. |
| `src/raft/WillemtRaftWaveApplication.h / .cc` | **WAVE subclass (~434 lines).** Extends `DemoBaseApplLayer` (Veins). Implements the same 6 virtual methods using Veins `WaveShortMessage` (WSM) on 802.11p / DSRC. Handles Veins send/receive. |
| `src/raft/RaftShared.h` | **Shared structs and enums.** Defines `ClusterPhase`, `LogEntryType`, `VehicleProposal`, `PassOrderEntry`, `PassBatch`, `PassScheduleEntry`, `VehicleLeftEntry`, `CommittedSchedule`. |
| `src/raft/RaftMetrics.h / .cc` | **JSON output collector.** Static singleton-like class. Vehicle 0 opens the results file; all vehicles append their record; the last vehicle auto-closes. |
| `src/raft/RaftWaveMessage.msg` | **OMNeT++ message definition** for the WAVE WSM payload. Auto-generates `RaftWaveMessage_m.h/cc`. |
| `src/raft/RaftLogger.h` | Logging macros wrapping `EV_INFO` / `EV_WARN`. |
| `third_party/raft/raft.h / .c` | **willemt/raft C library.** Pure C implementation of the Raft consensus algorithm. Compiled statically into the OMNeT++ binary. |
| `simulations/simple_intersection_N/omnetpp_udp.ini` | OMNeT++ config for UDP scenario (N = 4, 8, or 16). Sets radio, channel, INET params, RAFT timing, discovery wait, and cluster formation delay. |
| `simulations/simple_intersection_N/omnetpp_wave.ini` | Same for WAVE. Uses Veins and 802.11p channel settings. |
| `simulations/simple_intersection_N/intersection.rou.xml` | SUMO route file. 4 directions (W,S,E,N), vehicles start far (departPos 0–140 m), 20 m spacing. |
| `simulations/simple_intersection_N/intersection.launchd.xml` | Veins launchd config. Tells `veins_launchd` which SUMO config files to stage. |
| `run_simple_benchmark.sh` | **Simple benchmark runner.** Runs UDP and WAVE for 4, 8, 16 vehicles (3 iterations each), aggregates stats, runs `plot_comparison.py --simple`. |
| `plot_comparison.py` | Reads result folders and generates RAFT Decision Time + Throughput comparison plot (UDP vs WAVE). |

### Difference Between UDP and WAVE Subclasses

| Aspect | UDP (`WillemtRaftApplication`) | WAVE (`WillemtRaftWaveApplication`) |
|--------|-------------------------------|-------------------------------------|
| **Base class** | `VeinsInetApplicationBase` | `DemoBaseApplLayer` |
| **Radio** | IEEE 802.11a, CH36 (5.18 GHz), INET UdpSocket | IEEE 802.11p, CH178 (5.89 GHz) DSRC, Veins WSM |
| **Unicast** | `UdpSocket::sendTo(packet, destAddr, port)` | `sendDirect()` or `sendDown()` with dest MAC |
| **Broadcast** | `UdpSocket::sendTo(packet, broadcast, port)` | `sendWSM()` with broadcast MAC |
| **Packet type** | INET `Packet` with `UdpHeader` + `BytesChunk` | Veins `RaftWaveMessage` (WSM subclass) |
| **Mobility** | `VeinsInetMobility` — wraps Veins TraCI | `TraCIMobility` — direct Veins TraCI |
| **Discovery beacon fix** | Beacons slow to 2.0 s during `PHASE_COORDINATION` | Same fix applied |
| **Protocol logic** | 100% in `RaftAppBase` | 100% in `RaftAppBase` |

---

## 1. The Logic

### 1a. How Cars Talk and Stop

#### Vehicle Identity

```
myId_         = getParentModule()->getIndex()   // 0-based: 0, 1, 2, 3...
myRaftNodeId_ = myId_ + 1                       // 1-based: 1, 2, 3, 4...
```

The willemt/raft library requires 1-indexed node IDs; `myRaftNodeId_` is always `myId_ + 1`.

#### Startup (both subclasses)

At simulation start, every vehicle:

1. Reads all parameters from `.ini` (`totalVehicles`, `electionTimeoutBaseMs`, etc.)
2. Sets `clusterPhase_ = PHASE_DISCOVERY`
3. Schedules three recurring timers:
   - **`checkTimer_`** — fires every 50 ms; drives stop detection, queue advancement, exit detection
   - **`discoveryTimer_`** — fires every `uniform(0, discoveryBeaconInterval)` (default 0.3 s); sends peer-discovery beacons when stopped
   - **`raftPeriodicTimer_`** — fires every 20 ms (only after cluster formed); ticks the RAFT library
4. Adds all peer IDs (0 … N-1) to `activeVehicles_`

#### Discovery Beacons

Every beacon is a small broadcast:

```
Payload: [int32 myId][uint8 clusterPhase]
Msg type: 0x10 (DISCOVERY_BEACON)
```

On receipt: add sender to `discoveredPeers_`. The cluster trigger fires after a wait (see §1b).

#### Stopping at the Intersection

`checkAndStopAtIntersection()` is called by `checkTimer_` every 50 ms:

```
1. Is current road one of approachEdges_?
2. getDistanceToJunction() = laneLength - lanePosition   [TraCI]
3. If distance <= intersectionStopDistance_ (5 m in simple intersection):
     setSpeedMode(0)       // disable all speed control
     setSpeed(0)           // hard stop
     hasStoppedAtIntersection_ = true
     timeStopped_ = NOW
4. Queued vehicles (dist < 4×stopDistance, speed < 1.5 m/s): same flags, no hard stop
```

**Note:** `intersectionStopDistance_` is 5 m in the simple intersection configs. Queued-vehicle zone = 5–20 m (4× stop distance).

#### Queue Advancement

For vehicles behind the front vehicle:

```
checkAndAdvanceInQueue() every 50 ms:
  leader, gap = TraCI.getLeader(4 × stopDistance)   // 20 m with stopDist=5
  if gap > stopDistance:
    setSpeedMode(31)    // restore SUMO car-following
    setSpeed(-1)        // let SUMO drive
  // Vehicle accordions forward, then re-stops automatically
```

---

### 1b. Leader Election Process

#### Cluster Formation Trigger

```
checkClusterTrigger() (called by discoveryTimer_ when stopped):
  Fires when ALL of:
    - hasStoppedAtIntersection_ == true
    - clusterPhase_ == PHASE_DISCOVERY
    - (NOW - timeStopped_) >= discoveryWaitMs_ + clusterFormationDelayMs_

  members = discoveredPeers_ ∪ {myId_}
  → calls formCluster(members)
```

**Simple intersection config:** `discoveryWaitMs = 5000`, `clusterFormationDelayMs = 2000` → first vehicle waits **7 s** after stopping before forming. This gives late-arriving vehicles time to stop and exchange discovery beacons so the cluster forms with all vehicles together.

#### `formCluster(members)`

```
1. raftServer_ = raft_new()
2. For each member vehicle id v:
     raft_add_node(raftServer_, udata=v, nodeId=v+1, is_self=(v==myId_))
3. Register callbacks (see §RAFT Callbacks below)
4. Set election timeout = electionTimeoutBaseMs_ + uniform(0, electionTimeoutJitterMs_)
5. Set request timeout = requestTimeoutMs_
6. clusterPhase_ = PHASE_COORDINATION
7. timeClusterFormed_ = NOW
8. broadcastClusterForm()            // msg type 0x11, payload=[numMembers][id0][id1]...
9. Schedule CLUSTER_FORM retries at +300, +700, +1200, +1800 ms (handles packet loss)
10. onClusterFormed() → start raftPeriodicTimer_
```

#### RAFT Election (inside the willemt/raft library)

Every 20 ms, `raft_periodic(raftServer_, elapsed_ms)` is called. Internally:

```
Follower state:
  Decrement election countdown by elapsed_ms
  When countdown ≤ 0:
    → become CANDIDATE
    → increment current_term
    → call send_requestvote callback for every other node

send_requestvote callback → doSendRequestVote():
  Serialize msg_requestvote_t struct
  sendRaftUnicast(targetId, 0x20, data)   // msg type: RAFT_REQUEST_VOTE

On receiving 0x20:
  raft_recv_requestvote(server, fromNode, &msg, &response)
  sendRaftUnicast(fromId, 0x21, response) // msg type: RAFT_REQUEST_VOTE_RESPONSE

On receiving 0x21:
  raft_recv_requestvote_response(server, fromNode, &response)
  If quorum votes received → become LEADER
  onBecameLeader() called
```

**Key**: Election timeout is **randomised per vehicle** so one vehicle's timeout fires first, wins the election before others even try. With 4 vehicles at `280 ± 160 ms` jitter, the fastest timeout wins in ~280–440 ms after cluster formation.

---

### 1c. Decision Process

#### Step 1 — Status Request

After `arrivalWaitTimeMs_` (default 300 ms for 4 vehicles) the leader broadcasts:

```
msg type: 0x30 (COORD_STATUS_REQUEST)
payload:  [int32 leaderId]
```

Each follower responds with its `VehicleProposal`:

```
msg type: 0x31 (COORD_STATUS_RESPONSE)
payload:  struct VehicleProposal {
    int    vehicleId
    char   laneEdgeId[64]    // current SUMO edge
    double positionOnLane    // metres from lane start
    double speed
    int    laneIndex         // 0=West 1=South 2=East 3=North
    int    intendedTurn      // 0=STRAIGHT 1=LEFT 2=RIGHT
    bool   isFirstInLane     // no vehicle ahead in same lane
    int    blockedByVehicleId
    double waitingTimeMs     // time since stopped
    double distanceToJunction
}
```

#### Step 2 — Pass Order Generation (`proposePassOrder`)

Once all proposals collected (or `statusCollectionTimeoutMs_` expires):

**Sort proposals:**

```
Priority:
  1. isFirstInLane (front vehicles first)
  2. waitingTimeMs (larger wait → higher priority, only if diff > 500 ms)
  3. laneIndex (ascending, for tie-breaking)
  4. distanceToJunction (ascending within lane)
```

**Greedy batch construction:**

```
Repeat until pool empty (max 16 batches):
  1. Pick primary: first unblocked vehicle in sorted pool
  2. Start new batch with primary
  3. Scan remaining vehicles, add if:
     a. Their blocker is already scheduled (dependency met)
     b. No movement conflict with anyone already in this batch
  4. Conflict check: movementsConflict(laneA, turnA, laneB, turnB)
     - Same lane → CONFLICT
     - Opposing lanes (laneA+laneB == 2 or 4), both STRAIGHT → NO CONFLICT
     - Opposing lanes, both LEFT → NO CONFLICT
     - Everything else → CONFLICT
```

Example output for 4 vehicles (W, S, E, N all going straight):

```
Batch 0: [veh0]       // West, first in lane
Batch 1: [veh1]       // South, first in lane
Batch 2: [veh2, veh3] // East+North — opposing pair, no conflict
```

#### Step 3 — RAFT Replication

```
Leader calls raft_recv_entry(raftServer_, entry, &response)
  entry.type = PASS_ORDER
  entry.data = serialized PassScheduleEntry

→ send_appendentries callback fires:
  msg type: 0x22 (RAFT_APPEND_ENTRIES)
  payload:  [term][prevLogIdx][prevLogTerm][leaderCommit][numEntries]
            [for each entry: term, id, type, dataLen, data]

Followers:
  raft_recv_appendentries(server, fromNode, &msg, &response)
  Append entry to local log
  msg type: 0x23 (RAFT_APPEND_ENTRIES_RESPONSE)

Leader: when quorum (⌊N/2⌋ + 1) followers have responded:
  commitIdx advances → applylog callback fires on all replicas
```

#### Step 4 — Apply Log (`doApplyLog`)

```
entry.type == PASS_ORDER:
  committedSchedule_ = passed schedule
  hasCommittedOrder_ = true
  timeOrderCommitted_ = NOW
  → applyCommittedPassOrder()
```

---

### 1d. Car Leaving Process

#### Batch Execution

```
applyCommittedPassOrder():
  Find myBatch_ = batch index that contains myId_

  if myBatch_ == currentBatch_ (0):
    resumeMovement() immediately

  if myBatch_ > currentBatch_:
    Wait for batch (myBatch_-1) to complete
    Safety timer: 6 s × myBatch_ (handles lost VEHICLE_LEFT messages)

resumeMovement():
  timeStartedMoving_ = NOW
  setSpeedMode(0)
  setParameter("jmIgnoreFoeProb", "1.0")    // ignore yielding
  setParameter("jmIgnoreFoeSpeed", "100")   // ignore foe speed
  setParameter("jmTimegapMinor", "0.0")     // ignore gap to superior vehicles
  setSpeed(maxSpeed_)                        // drive at maximum
```

#### Detecting Intersection Exit

`checkIfLeftIntersection()` runs every 50 ms:

```
hasPassedIntersectionEdge():
  → currentRoad ∈ exitEdges_?  YES → return true
  → OR: timeStartedMoving_ > 0 AND currentRoad ∉ (approachEdges_ ∪ internalEdges_)
```

When true:

```
hasPassedIntersection_ = true
timePassed_ = NOW
sendVehiclePassed()   → broadcast 0x33 with [uint8 myId]
sendVehicleLeft()     → if leader: propose VEHICLE_LEFT RAFT entry
                         if follower: broadcast 0x34 (COORD_VEHICLE_LEFT)
outputMetricsJSON()
finish()
```

#### Batch Advancement (leader side)

```
On receiving VEHICLE_LEFT for vehicle v in batch B:
  vehiclesLeftInBatch_.insert(v)

checkBatchAdvance():
  if all vehicles in currentBatch_ are in vehiclesLeftInBatch_:
    currentBatch_++
    vehiclesLeftInBatch_.clear()
    if myBatch_ == currentBatch_:
      resumeMovement()   // this node is next
```

---

## 2. How Each Time Is Calculated

All timestamps are in **simulation milliseconds** from `simTime() * 1000`.

### Timestamps Recorded

| Timestamp | Set When |
|-----------|----------|
| `timeArrived_` | `startApplication()` / `initialize()` — simulation start |
| `timeStopped_` | `checkAndStopAtIntersection()` fires — vehicle hard-stops at approach |
| `timeClusterFormed_` | Inside `formCluster()` after `clusterPhase_ = PHASE_COORDINATION` |
| `timeElected_` | Inside `onBecameLeader()` — RAFT library declared this node leader |
| `timeOrderCommitted_` | Inside `doApplyLog()` when `entry.type == PASS_ORDER` |
| `timeStartedMoving_` | Inside `resumeMovement()` — vehicle released from stop |
| `timePassed_` | Inside `checkIfLeftIntersection()` — vehicle confirmed off intersection |

### Durations Derived

```
raftDecisionTime  = max(order_committed) - min(cluster_formed)   [across RAFT vehicles]
  → From cluster formation until pass order is committed (leader election + replication)

totalWaitTime     = timePassed_ - timeStopped_
  → Full intersection visit: waiting + crossing

transitTime       = timePassed_ - timeStartedMoving_
  → Time physically driving through the junction

throughput        = N_raft / (max(passed) - min(cluster_formed))   [RAFT vehicles only, time in s]
  → Throughput during RAFT coordination window only (excludes discovery wait and pre-formation delay)
```

### RAFT Timing Parameters (Simple Intersection)

```ini
# Fixed values in simple_intersection_4/8/16 omnetpp_*.ini:
electionTimeoutBaseMs = 500
electionTimeoutJitterMs = 1000
requestTimeoutMs = 200
discoveryWaitMs = 5000
clusterFormationDelayMs = 2000
intersectionStopDistance = 5m
```

Election timeout per vehicle = 500 + uniform(0, 1000) ms → 500–1500 ms. The cluster formation wait is 7 s (5 s discovery + 2 s settling) after the first vehicle stops.

---

## 3. RAFT Callbacks (How the Library Talks to Our Code)

```
raft_set_callbacks(server, &cbs, this):

cbs.send_requestvote        → doSendRequestVote(server, udata, node, msg)
  Invoked when becoming candidate; serializes msg_requestvote_t
  → sendRaftUnicast(targetId, 0x20, data)

cbs.send_appendentries      → doSendAppendEntries(server, udata, node, msg)
  Invoked by leader every requestTimeoutMs_; serializes variable-length entries
  → sendRaftUnicast(targetId, 0x22, data)

cbs.log_offer               → doLogOffer(server, udata, entry, entryIdx)
  Called when entry is offered to log; we always accept (return 0)

cbs.applylog                → doApplyLog(server, udata, entry, entryIdx)
  Called when entry is committed (replicated to quorum)
  Dispatches on entry type:
    STATUS_REPORT → store collected statuses
    PASS_ORDER    → apply schedule, begin batch execution
    VEHICLE_LEFT  → remove vehicle from active set, check batch advance
    PASS_COMMAND  → (legacy) resume movement

cbs.persist_vote            → persistVote(server, udata, voted_for)
  No-op (no durable storage in simulation)

cbs.log                     → raftLog(server, udata, buf)
  Debug logging (no-op in release build)
```

---

## 4. Message Types Summary

| Hex | Name | Direction | Payload |
|-----|------|-----------|---------|
| `0x10` | `DISCOVERY_BEACON` | Broadcast | `[int32 myId][uint8 phase]` |
| `0x11` | `CLUSTER_FORM` | Broadcast | `[uint8 numMembers][int32 id] × N` |
| `0x12` | `CLUSTER_EXISTS` | Broadcast | Same as CLUSTER_FORM |
| `0x20` | `RAFT_REQUEST_VOTE` | Broadcast (all vehicles receive; non-target ignores) | `msg_requestvote_t` (C struct) |
| `0x21` | `RAFT_REQUEST_VOTE_RESPONSE` | Broadcast (same) | `msg_requestvote_response_t` |
| `0x22` | `RAFT_APPEND_ENTRIES` | Broadcast (same) | Variable — base header + log entries |
| `0x23` | `RAFT_APPEND_ENTRIES_RESPONSE` | Broadcast (same) | `msg_appendentries_response_t` |
| `0x30` | `COORD_STATUS_REQUEST` | Broadcast | `[int32 leaderId]` |
| `0x31` | `COORD_STATUS_RESPONSE` | Broadcast (same — non-leader ignores) | `struct VehicleProposal` |
| `0x33` | `COORD_VEHICLE_PASSED` | Broadcast | `[uint8 vehicleId]` |
| `0x34` | `COORD_VEHICLE_LEFT` | Broadcast | `struct VehicleLeftEntry {vehicleId, batchId}` |
| `0x35` | `COORD_VEHICLE_LEFT_REBROADCAST` | Broadcast | Same as 0x34 |

**All messages in both UDP and WAVE are broadcast at the network layer.** UDP uses IP multicast address `224.0.0.1`; WAVE sets `recipientAddress = -1`. `sendRaftUnicast` is a misnomer — it sends to the same broadcast destination as `sendRaftBroadcast`. The only distinction is an application-layer `targetId` field; non-target vehicles drop the packet silently after receiving it.

---

## 5. How the Benchmark Script Executes Everything

### run_simple_benchmark.sh — Step by Step

```bash
./run_simple_benchmark.sh 3
```

**Prerequisite:** `veins_launchd` and SUMO must already be running.

```
1. FOR EACH VEHICLE COUNT (4, 8, 16):
   a. UDP: cd simulations/simple_intersection_N/
      Run: ./src/benchmark -u Cmdenv -n "$NED_PATH" omnetpp_udp.ini
           --seed-set=$i --**.app[0].resultsFile="results/simple_udp_Nveh/run_i/raft_results.json"
   b. WAVE: same dir, omnetpp_wave.ini, --**.appl.resultsFile=...

2. AGGREGATE (Python inline):
   For each result dir (simple_udp_4veh, simple_raftwave_4veh, ...):
     - RAFT decision time = max(order_committed) - min(cluster_formed)
     - Throughput = N_raft / (max(passed) - min(cluster_formed)) for RAFT vehicles only
     - Save aggregate_stats.json

3. PLOT:
   python3 plot_comparison.py --simple
   Generates: RAFT Decision Time, System Throughput (2 subplots)
   Output: results/simple_wave_vs_udp_comparison.png
```

### NED Path Construction

```bash
NED_PATH="$INET_DIR/src:$VEINS_DIR/src:$VEINS_INET_DIR/src:./src"
```

This tells OMNeT++ where to find `.ned` files for INET modules, Veins modules, Veins-INET bridge, and the benchmark's own modules.

### SUMO Coordination

OMNeT++ (via Veins `TraCIScenarioManager`) connects to `veins_launchd` on port 9999. `veins_launchd` starts SUMO, stages config files to a temp directory, and acts as a proxy. Without `DISPLAY=:1`, SUMO's GUI cannot launch.

---

## 6. Full State Transition Diagram

```
t = 0 ms
│
├── PHASE_DISCOVERY
│   ├── checkTimer (50 ms): detect stop, queue advance, exit check
│   ├── discoveryTimer (~0.3 s): when stopped, broadcast DISCOVERY_BEACON, call checkClusterTrigger()
│   └── receive DISCOVERY_BEACON: add to discoveredPeers_
│
│   [when: stopped AND (NOW - timeStopped_) >= discoveryWaitMs + clusterFormationDelayMs (7 s)]
│
├── formCluster() → PHASE_COORDINATION  (~7 s after first vehicle stops)
│   ├── raft_new(), raft_add_node() for all members
│   ├── raft_set_callbacks()
│   ├── broadcast CLUSTER_FORM (0x11), retries @ +300/700/1200/1800 ms
│   └── start raftPeriodicTimer (20 ms)
│
│   [raftPeriodicTimer ticking every 20 ms]
│
├── RAFT ELECTION  (~2100–3000 ms)
│   ├── raft_periodic() advances election countdown
│   ├── First vehicle to time out → CANDIDATE
│   │   ├── send REQUEST_VOTE (0x20) to all
│   │   └── receive REQUEST_VOTE_RESPONSE (0x21)
│   └── Quorum votes → LEADER
│       └── onBecameLeader()
│           ├── wasElectedLeader_ = true
│           ├── timeElected_ = NOW
│           └── schedule statusRequest after arrivalWaitTimeMs_
│
├── STATUS COLLECTION  (~3300–4100 ms)
│   ├── leader sends STATUS_REQUEST (0x30)
│   ├── followers send STATUS_RESPONSE (0x31) with VehicleProposal
│   └── leader: when all received (or timeout) → proposePassOrder()
│
├── RAFT REPLICATION  (~4100–4400 ms)
│   ├── leader: raft_recv_entry(PASS_ORDER)
│   ├── leader sends APPEND_ENTRIES (0x22) to all followers
│   ├── followers: raft_recv_appendentries → APPEND_ENTRIES_RESPONSE (0x23)
│   └── quorum responds → commit index advances → applylog fires
│
├── BATCH EXECUTION  (~4400 ms onward)
│   ├── applyCommittedPassOrder()
│   │   ├── batch 0 vehicles: resumeMovement() immediately
│   │   └── batch 1+ vehicles: wait for previous batch
│   │
│   ├── Batch 0 crossing:
│   │   ├── vehicle exits → sendVehiclePassed (0x33)
│   │   ├── sendVehicleLeft (RAFT entry if leader, 0x34 if follower)
│   │   └── leader: VEHICLE_LEFT committed → checkBatchAdvance()
│   │       └── currentBatch_++ → batch 1 vehicles: resumeMovement()
│   │
│   └── Repeat for each batch...
│
└── METRICS + FINISH  (after last vehicle passes)
    ├── outputMetricsJSON() → append to raft_results.json
    ├── RaftMetrics auto-closes file when vehiclesCompleted_ >= totalVehicles_
    └── finish() → cancel all timers, free raftServer_
```

---

## 7. Fallback Mode

When RAFT fails (too many retries, or global timeout):

```
Triggers:
  1. failedElectionCount_ >= maxFailedElections_ (5)
  2. Global timeout: cluster formed but no order committed within
     arrivalWait + statusTimeout + 4×electionTimeout + 3 s

handleFallback():
  isFallbackMode_ = true
  coordinationMethod_ = "fallback"
  timeOrderCommitted_ = NOW  // mark time for metrics

  if wayOfSight_ OR no blocker:
    resumeMovement() immediately
  else:
    delay = 1000 + positionInLane × 2000 ms
    schedule resumeMovement() after delay
```

Fallback ensures vehicles never deadlock — they always eventually move, even without consensus.

---

## 8. Key Differences: UDP vs WAVE in Practice

| Characteristic | UDP (802.11a) | WAVE (802.11p) |
|----------------|--------------|----------------|
| **Channel / Frequency** | CH36 = 5.18 GHz (802.11a UNII-1) | CH178 = 5.89 GHz (DSRC CCH) |
| **Bandwidth** | 20 MHz | 10 MHz |
| **Tx power** | 200 mW (23 dBm, OBU-class) | 200 mW (23 dBm, OBU-class) |
| **Receiver sensitivity** | -82 dBm (802.11a std) | -88 dBm (802.11p std) |
| **Standard** | IEEE 802.11a | IEEE 802.11p |
| **RAFT logic** | Identical (in RaftAppBase) | Identical (in RaftAppBase) |
| **Beacon during RAFT** | 2.0 s | 2.0 s |
| **RAFT time (4 veh)** | ~2.0 s | ~1.5 s (WAVE faster: better connectivity at low load) |
| **RAFT time (8 veh)** | ~9.4 s | ~6.5 s (WAVE faster) |
| **RAFT time (16 veh)** | ~12.1 s | ~14.5 s (UDP faster: 20 MHz vs 10 MHz at high load) |

**Scaling behaviour:** At 4–8 vehicles, WAVE’s 10 dB better effective sensitivity (vs older -82 dBm UDP) helped connectivity across opposite approaches (~350 m). Both use standards-aligned sensitivity (UDP -82 dBm, WAVE -88 dBm) and matched OBU power (200 mW). At 16 vehicles, channel capacity matters more—UDP’s 20 MHz outperforms WAVE’s 10 MHz.

### Cluster Merge Rule

When receiving `CLUSTER_EXISTS` from another cluster: merge only if the incoming cluster is **strictly larger**. Equal-sized clusters do not merge (avoids deadlock; radio partitioning is handled by fallback).
