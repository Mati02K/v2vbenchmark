# RaftDiscovery — Function Call Flowchart

All functions live in `src/raft/RaftDiscovery.cc` unless marked with their source file.

---

## Phase 1 — DISCOVERY: Peer beacons

```mermaid
flowchart TD
    A([Discovery timer fires every ~100ms\nsubclass scheduleAt]) --> B[sendPeerBeacon]
    B --> B1[updateMyProposal\nRaftUtilities]
    B --> B2[CryptoAuth::signProposal\nlib/crypto]
    B --> B3[sendRaftBroadcast 0x10\ntransport]

    C([Incoming 0x10 packet]) --> D[handlePeerBeacon]
    D --> D1[CryptoAuth::verifyCert\nlib/crypto]
    D --> D2[CryptoAuth::verifyProposalSignature\nlib/crypto]
    D --> D3[vehicleDB_ updated\nwith verified isPriority]
```

---

## Phase 2 — STOP DETECTION: checkAndStopAtIntersection (every 50ms)

```mermaid
flowchart TD
    A([Check timer fires every 50ms\nsubclass scheduleAt]) --> B[checkAndStopAtIntersection]

    B --> C[updateLaneLeaderFlag]
    C --> C1[isLaneLeaderByTraci\nRaftUtilities]
    C --> C2{Lane leader\npromotion?}
    C2 -- yes + unscheduled vehicles --> C3[startNewRound]
    C2 -- no --> DONE1[ ]

    B --> D{Road type?}

    D -- internal ':cluster' road --> E[handleClusterJunctionStop]
    E --> E1[stopVehicle\nRaftUtilities]
    E --> E2[calculateWayOfSight\nRaftUtilities]
    E --> E3[onFirstStoppedAtIntersection]
    E --> E4[scheduleLeaderStatusRequest\nRaftUtilities]

    D -- approach edge, dist <= stopDist --> F[handleFrontStop]
    F --> F1[calculateWayOfSight\nRaftUtilities]
    F --> F2[onFirstStoppedAtIntersection]
    F --> F3{Can go now?}
    F3 -- yes --> F4[resumeMovement\nRaftUtilities]
    F3 -- no --> F5[stopVehicle\nRaftUtilities]
    F5 --> F6[scheduleLeaderStatusRequest\nRaftUtilities]

    D -- queued, speed < 1.5 m/s --> G[handleQueuedStop]
    G --> G1[calculateWayOfSight\nRaftUtilities]
    G --> G2[onFirstStoppedAtIntersection]
    G --> G3[scheduleLeaderStatusRequest\nRaftUtilities]

    D -- road change --> H[updateRoadTracking]
    H --> H1[intersectionEdge_ / myLaneIndex_\ncached for later use]
```

---

## Phase 3 — FORMATION: onFirstStoppedAtIntersection → formCluster

```mermaid
flowchart TD
    A[onFirstStoppedAtIntersection] --> B[updateLaneLeaderFlag]

    A --> C{isLaneLeader_?}
    C -- yes --> D[sendLaneLeaderBeacon]
    C -- yes --> E[scheduleClusterFormationLoop]
    C -- no --> WAIT[wait for\nCLUSTER_FORM_BROADCAST]

    A --> F[scheduleOneshotMs fallbackClusterTimeoutMs_\nif RAFT not started → handleFallback]

    D --> D1[sendRaftBroadcast 0x15\nCLUSTER_JOIN_INVITE]
    D --> D2[tryFormClusterFromCollected]

    E --> E1{every 500ms:\nraftStarted_?}
    E1 -- no --> D
    E1 -- no --> E
    E1 -- yes --> STOP1[loop stops]

    subgraph Receive side
        R1([Incoming 0x15\nCLUSTER_JOIN_INVITE]) --> R2[handleClusterJoinInvite]
        R2 --> R3{isLaneLeader_?}
        R3 -- yes --> R4[tryFormClusterFromCollected]
        R3 -- no --> SKIP[ ]
    end

    D2 --> Q{all lanes\nrepresented?}
    R4 --> Q
    Q -- no --> WAIT2[wait for more beacons]
    Q -- yes --> FC[tryFormClusterFromCollected]

    FC --> FC1[sendRaftBroadcast 0x16\nCLUSTER_FORM_BROADCAST]
    FC --> FC2[raftStarted_ = true]
    FC --> FC3[formCluster members]

    subgraph Receive side 2
        B2([Incoming 0x16\nCLUSTER_FORM_BROADCAST]) --> H2[handleClusterFormBroadcast]
        H2 --> H3{myId_ in\nmembers?}
        H3 -- yes --> H4[raftStarted_ = true]
        H4 --> H5[formCluster members]
        H3 -- no --> SKIP2[ ]
    end
```

---

## Phase 3 continued — formCluster internals

```mermaid
flowchart TD
    A[formCluster members] --> B{clusterPhase_\n== PHASE_DISCOVERY?}
    B -- no --> RETURN[return early\nalready formed]
    B -- yes --> C[clusterPhase_ = PHASE_FORMATION\ntimeClusterFormed_ = NOW]

    C --> D[raft_new\nwillemt/raft C library]
    D --> E[raft_set_callbacks\nsendRequestVote, sendAppendEntries,\nlogOffer, applylog, persistVote]
    E --> F[raft_set_election_timeout\nbase + random jitter]
    F --> G[raft_add_node\nfor each member]
    G --> H[mark vehiclesLeftBeforeFormed_\ninactive in RAFT]
    H --> I[clusterPhase_ = PHASE_COORDINATION]
    I --> J[onClusterFormed\npure virtual → subclass]
    J --> K[scheduleAt now+20ms raftPeriodicTimer_\nstarts RAFT heartbeat loop]
```

---

## Multi-round — startNewRound (called from updateLaneLeaderFlag)

```mermaid
flowchart TD
    A[updateLaneLeaderFlag detects\nlane leader promotion] --> B{hasUnscheduledVehicles?}
    B -- no --> DONE[no new round needed]
    B -- yes --> C[startNewRound]

    C --> C1[roundNumber_++\nseekingNewCluster_ = true]
    C --> C2[scheduledVehicles_ built from\ncommittedSchedule_ previous round]
    C --> C3[raft_free old raftServer_]
    C --> C4[reset ALL raft + coordination state\nraftStarted_, passOrderProposed_,\nhasCommittedOrder_, isLeader_, etc.]
    C4 --> C5[scheduleOneshotMs 500ms]
    C5 --> C6[seekingNewCluster_ = false]
    C6 --> C7[sendLaneLeaderBeacon]
    C6 --> C8[scheduleClusterFormationLoop]
    C7 --> FORM[→ re-enters Phase 3 formation]
    C8 --> FORM
```

---

## Summary — Entry points and handoff to RaftCore

| Entry point | Called by | Hands off to |
|---|---|---|
| `sendPeerBeacon` | subclass discovery timer | — |
| `handlePeerBeacon` | subclass msg dispatcher | — |
| `checkAndStopAtIntersection` | subclass check timer (50ms) | `onFirstStoppedAtIntersection` |
| `handleClusterJoinInvite` | subclass msg dispatcher | `tryFormClusterFromCollected` |
| `handleClusterFormBroadcast` | subclass msg dispatcher | `formCluster` |
| `formCluster` | `tryFormClusterFromCollected` or `handleClusterFormBroadcast` | `onClusterFormed` → starts `raftPeriodicTimer_` → **RaftCore** takes over |
