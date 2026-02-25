# RAFT Intersection Coordination Algorithm

This document explains how the RAFT-based intersection coordination system works, from the third-party library integration to the complete execution flow and timing calculations.

---

## Table of Contents
1. [Third-Party Library Integration](#third-party-library-integration)
2. [System Architecture](#system-architecture)
3. [Complete Execution Flow](#complete-execution-flow)
4. [Timing Calculations](#timing-calculations)
5. [RAFT Protocol Steps](#raft-protocol-steps)

---

## Third-Party Library Integration

### Willemt/RAFT Library

**Source**: The code uses the [willemt/raft](https://github.com/willemt/raft) library, a C implementation of the RAFT consensus algorithm.

**Integration Method**:
- **Location**: `third_party/raft/` directory
- **Files included**:
  - `raft.h` - Main RAFT API
  - `raft.c` - Core RAFT implementation
  - `raft_types.h` - Data structures
  - `raft_private.h` - Internal structures

**How it's used**:
```cpp
// In WillemtRaftApplication.h
extern "C" {
#include "../../third_party/raft/raft_types.h"
#include "../../third_party/raft/raft.h"
}
```

The library is **statically linked** into the OMNeT++ simulation, not cloned at runtime. It provides the core RAFT consensus logic while our code handles:
- Network communication (via OMNeT++)
- Vehicle coordination logic
- Intersection-specific decision making

---

## System Architecture

### Key Components

```
┌─────────────────────────────────────────────────────────┐
│          WillemtRaftApplication.cc                      │
│  (OMNeT++ Application Layer - Per Vehicle)              │
├─────────────────────────────────────────────────────────┤
│  - Vehicle state management                             │
│  - Network packet handling                              │
│  - Intersection detection                               │
│  - Coordination logic                                   │
└────────────┬────────────────────────────────────────────┘
             │
             ├──► Willemt RAFT Library (third_party/raft/)
             │    - Leader election
             │    - Log replication
             │    - Consensus protocol
             │
             ├──► Veins/INET (OMNeT++ frameworks)
             │    - Vehicle mobility (SUMO integration)
             │    - Network simulation (IEEE 802.11p)
             │
             └──► TraCI (SUMO control)
                  - Vehicle speed control
                  - Position queries
                  - Lane information
```

---

## Complete Execution Flow

### Phase 1: Initialization

**File**: `WillemtRaftApplication::initialize()`

```
1. Read parameters from .ned file
   - totalVehicles, electionTimeoutBaseMs, etc.
   - statusCollectionTimeoutMs (scaled: 200ms + vehicles × 12.5ms)

2. Initialize RAFT server
   - Create raft_server_t instance
   - Set callbacks for network I/O
   - Configure as follower initially

3. Set up vehicle info
   - Extract vehicle ID from OMNeT++ module
   - Determine lane (N/S/E/W) based on ID
   - Get TraCI interface for SUMO control

4. Open results file
   - Create raft_results.json for metrics
```

### Phase 2: Vehicle Approaches Intersection

**File**: `WillemtRaftApplication::handlePositionUpdate()`

```
1. Check if near intersection (every position update)
   - Query current position via TraCI
   - Calculate distance to intersection center

2. When within 50m:
   - Set hasStoppedAtIntersection_ = true
   - Record timeStopped_ = simTime()
   - Call mobility_->setSpeed(0) to stop vehicle

3. Start RAFT coordination
   - If not already in RAFT cluster, join
   - Begin periodic RAFT processing (20ms intervals)
```

### Phase 3: RAFT Leader Election

**File**: `WillemtRaftApplication::processRaftPeriodic()`

```
1. Call raft_periodic() every 20ms
   - Advances RAFT state machine
   - Handles election timeouts
   - Processes pending messages

2. Election timeout triggers (random 150-300ms)
   - Follower → Candidate
   - Candidate requests votes from other vehicles
   - Sends RequestVote RPC via OMNeT++ packets

3. Vote collection
   - Each vehicle votes for first candidate
   - Candidate needs majority (quorum)
   - Winner becomes leader

4. Track election
   - When term increases: electionRounds_++
   - If became leader: timeElected_ = simTime()
```

### Phase 4: STATUS Collection (Leader Only)

**File**: `WillemtRaftApplication::requestStatusFromVehicles()`

```
1. Leader broadcasts STATUS_REQUEST
   - Sends packet to all stopped vehicles
   - Asks for wayOfSight (can see through intersection?)

2. Vehicles respond with STATUS_RESPONSE
   - Include wayOfSight boolean
   - Sent back to leader

3. Leader collects responses
   - Timeout: 200ms + (vehicles × 12.5ms)
   - Early break if all vehicles respond
   - Stores in collectedWayOfSight_ map

4. When complete:
   - Call collectStatusAndDecide()
```

### Phase 5: RAFT Consensus - STATUS_REPORT

**File**: `WillemtRaftApplication::proposeStatusReport()`

```
1. Leader creates STATUS_REPORT entry
   - Packages all vehicle statuses
   - Creates RAFT log entry

2. Propose to RAFT
   - raft_recv_entry() submits to log
   - RAFT replicates to followers via AppendEntries RPC

3. Followers receive and apply
   - raft_apply_entry() callback triggered
   - Each vehicle stores committedStatuses_

4. Quorum commit
   - When majority acknowledges
   - Entry becomes committed
   - All vehicles have same status view
```

### Phase 6: Leader Decides Pass Order

**File**: `WillemtRaftApplication::proposePassOrder()`

```
1. Leader calculates optimal order
   - Prioritize vehicles with wayOfSight=true
   - Sort by lane (N, S, E, W)
   - Handle blocked vehicles

2. Create PASS_ORDER entry
   - Array of vehicle IDs in pass order
   - Submit to RAFT log

3. RAFT replicates order
   - AppendEntries to all followers
   - Wait for quorum acknowledgment
```

### Phase 7: Order Execution

**File**: `WillemtRaftApplication::executePassOrder()`

```
1. When PASS_ORDER committed:
   - timeOrderCommitted_ = simTime()  ← RAFT DECISION TIME END
   - All vehicles have the order

2. Each vehicle finds its position
   - Search for myId_ in order array
   - Calculate delay: position × 150ms

3. First vehicle (position 0):
   - Starts immediately
   - resumeMovement() → setSpeed(13.89 m/s)
   - timeStartedMoving_ = simTime()

4. Other vehicles:
   - Wait for calculated delay
   - Then resumeMovement()
   - Sequential passing with 150ms gaps
```

### Phase 8: Crossing Intersection

**File**: `WillemtRaftApplication::handlePositionUpdate()`

```
1. Vehicle moves through intersection
   - Speed = 13.89 m/s (50 km/h)
   - TraCI controls actual movement

2. When past intersection:
   - Detect via position check
   - timePassed_ = simTime()
   - Set hasPassedIntersection_ = true

3. Write metrics to JSON
   - outputMetricsJSON()
   - Record all timestamps and durations
```

---

## Timing Calculations

### Timestamps Recorded

```cpp
// In WillemtRaftApplication.h
simtime_t timeArrived_;         // When vehicle spawned
simtime_t timeStopped_;         // When stopped at intersection
simtime_t timeElected_;         // When became leader (if applicable)
simtime_t timeOrderCommitted_;  // When PASS_ORDER committed to quorum
simtime_t timeStartedMoving_;   // When started crossing
simtime_t timePassed_;          // When cleared intersection
```

### Metrics Calculated

**File**: `WillemtRaftApplication::outputMetricsJSON()`

```cpp
// 1. RAFT Decision Time (NEW - most important!)
double raftDecisionTimeMs = (timeOrderCommitted_ - timeStopped_).dbl() * 1000.0;
// Measures: Leader election + STATUS collection + PASS_ORDER consensus
// This is the pure RAFT overhead

// 2. Total Wait Time (per vehicle)
double totalWaitTimeMs = (timeStartedMoving_ - timeStopped_).dbl() * 1000.0;
// Measures: RAFT decision + waiting for turn

// 3. Transit Time
double transitTimeMs = (timePassed_ - timeStartedMoving_).dbl() * 1000.0;
// Measures: Time to cross intersection

// 4. Election Time (leader only)
double electionTimeMs = (timeElected_ - timeStopped_).dbl() * 1000.0;
// Measures: Time to become leader
```

### Scenario-Level Metrics

**File**: `benchmark.sh`, `plot_comparison.py` (aggregate analysis)

**RAFT Decision Time** = leader election + decision writing to quorum only (cluster formation excluded):

```python
# RAFT timing = from cluster ready to last commit (NOT including cluster formation)
latest_commit = max(order_committed_times)
first_cluster = min(cluster_formed_times)
raft_decision = latest_commit - first_cluster
```

**Definition**: Cluster formation timing is never part of RAFT timings. RAFT timings = leader election + decision writing to quorum. The cluster formation phase (vehicles discovering each other) precedes RAFT and depends on physical arrival and radio range—it is not a RAFT protocol cost.

**Total Intersection Time** (unchanged):
```python
total_intersection = max(passed_times) - min(stopped_times)
```

**Why use LATEST commit?**
- RAFT consensus isn't complete until ALL vehicles have the order
- Last vehicle receiving order = true consensus completion

---

## RAFT Protocol Steps

### 1. Leader Election

```
Time: 0ms
┌─────────┐  ┌─────────┐  ┌─────────┐  ┌─────────┐
│Vehicle 0│  │Vehicle 1│  │Vehicle 2│  │Vehicle 3│
│Follower │  │Follower │  │Follower │  │Follower │
└─────────┘  └─────────┘  └─────────┘  └─────────┘

Time: 150ms (timeout)
┌─────────┐  
│Vehicle 1│ ──RequestVote──► All vehicles
│Candidate│  
└─────────┘  

Time: 180ms
┌─────────┐  
│Vehicle 1│ ◄──VoteGranted── All vehicles (quorum!)
│ LEADER  │  
└─────────┘  
```

### 2. STATUS Collection

```
Time: 180ms
┌─────────┐  
│Vehicle 1│ ──STATUS_REQUEST──► All vehicles
│ LEADER  │  
└─────────┘  

Time: 200ms
┌─────────┐  
│Vehicle 1│ ◄──STATUS_RESPONSE── All vehicles
│ LEADER  │     (wayOfSight data)
└─────────┘  
```

### 3. STATUS_REPORT Consensus

```
Time: 210ms
┌─────────┐  
│Vehicle 1│ ──AppendEntries(STATUS_REPORT)──► Followers
│ LEADER  │  
└─────────┘  

Time: 240ms
┌─────────┐  
│Vehicle 1│ ◄──Success── Quorum acknowledges
│ LEADER  │  
└─────────┘  
All vehicles apply STATUS_REPORT
```

### 4. PASS_ORDER Consensus

```
Time: 250ms
┌─────────┐  
│Vehicle 1│ ──AppendEntries(PASS_ORDER)──► Followers
│ LEADER  │     [0,1,2,3]
└─────────┘  

Time: 310ms (LAST vehicle receives)
┌─────────┐  ┌─────────┐  ┌─────────┐  ┌─────────┐
│Vehicle 0│  │Vehicle 1│  │Vehicle 2│  │Vehicle 3│
│Has order│  │Has order│  │Has order│  │Has order│
└─────────┘  └─────────┘  └─────────┘  └─────────┘
                                        ▲
                                        │
                            timeOrderCommitted_ = 310ms
                            RAFT Decision Time = 310 - 150 = 160ms
```

### 5. Sequential Execution

```
Time: 310ms
Vehicle 0 (position 0): delay = 0ms → starts immediately

Time: 460ms
Vehicle 1 (position 1): delay = 150ms → starts now

Time: 610ms
Vehicle 2 (position 2): delay = 300ms → starts now

Time: 760ms
Vehicle 3 (position 3): delay = 450ms → starts now
```

---

## Scaling with Vehicle Count

### Timeout Scaling

```cpp
// Base 200ms + 12.5ms per vehicle
statusCollectionTimeoutMs_ = 200 + (totalVehicles_ * 12.5);

// Examples:
// 4 vehicles:  250ms timeout
// 8 vehicles:  300ms timeout
// 16 vehicles: 400ms timeout
// 32 vehicles: 600ms timeout
```

**Why scale?**
- More vehicles = more STATUS messages
- More network traffic = higher collision probability
- Larger quorum = more consensus rounds needed
- Realistic simulation of network congestion

### Early Break Optimization

```cpp
// In handleStatusResponse()
if (statusResponseCount_ >= expectedResponses) {
    collectStatusAndDecide();  // Don't wait full timeout!
}
```

If all vehicles respond in 100ms but timeout is 600ms, we proceed immediately.

---

## Key Design Decisions

### 1. Why Two-Phase Consensus?

**Phase 1: STATUS_REPORT**
- Ensures all vehicles have same view of intersection state
- Prevents split-brain scenarios
- Leader can make informed decision

**Phase 2: PASS_ORDER**
- Ensures all vehicles agree on execution order
- Prevents conflicts and collisions
- Survives leader failures

### 2. Why Sequential Passing?

**Position-based delays** (position × 150ms):
- Prevents physical collisions
- Simpler than real-time coordination
- Deterministic and predictable
- Works even with network delays

### 3. Why Track Latest Commit?

**RAFT decision time = last vehicle receives order**:
- True measure of consensus completion
- Accounts for network propagation delays
- More realistic than "first vehicle" metric
- Shows actual scalability characteristics

---

## Summary

The RAFT intersection coordination system:

1. **Uses willemt/raft library** for core consensus protocol
2. **Integrates with OMNeT++/Veins** for network and mobility simulation
3. **Implements two-phase consensus** for safety and correctness
4. **Scales timeout with vehicle count** for realistic performance
5. **Tracks comprehensive timing metrics** for accurate benchmarking
6. **Ensures sequential execution** for collision avoidance

The result is a realistic simulation of distributed consensus for autonomous intersection management, with proper timing measurements that show how RAFT overhead scales with the number of vehicles.
