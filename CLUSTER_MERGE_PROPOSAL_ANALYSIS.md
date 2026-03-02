# Cluster Formation & Merge Proposal — Analysis Report

## Executive Summary

Your proposal replaces the current **size-based merge** (incoming must be strictly larger, with equal-size allowed as a special case) and **7-second wait** with a **timestamp + clusterId-based merge** and **5-second formation window**, with cluster invitations sent from startup. This report analyzes the current code, your proposed changes, and provides an implementation plan.

**Continuous broadcast until exit (your requirement):**

| Question | Answer |
|----------|--------|
| Is it already done? | **Partially.** The recurring discovery timer keeps sending until `hasPassedIntersection_`. The one-shot broadcasts in `formCluster()` stop at `hasCommittedOrder_`. |
| Is it achievable? | **Yes.** Use `!hasPassedIntersection_` as the only send gate; never gate on `hasCommittedOrder_`. Remove `!hasCommittedOrder_` from the one-shot conditions. |

---

## 1. Current System (As-Is)

### 1.1 Discovery Phase

| Component | Behavior |
|-----------|----------|
| **Discovery beacon** | Payload: `[myId(4B)][clusterPhase(1B)]`. Sent when `hasStoppedAtIntersection_` (UDP) or in PHASE_COORDINATION (cluster-exists). |
| **When sent** | Discovery timer fires every ~0.3 s. **Only when stopped** at intersection does it send beacons and call `checkClusterTrigger()`. |
| **discoveredPeers_** | Set of vehicle IDs from received beacons. |

### 1.2 Cluster Formation Trigger

```
checkClusterTrigger():
  - Requires: clusterPhase==DISCOVERY, hasStoppedAtIntersection, !clusterFormationScheduled
  - Waits: discoveryWaitMs (5s) + clusterFormationDelayMs (2s) = 7 seconds after FIRST vehicle stops
  - Then: formCluster(discoveredPeers ∪ {myId})
```

**Important:** Beacons are **not** sent from startup. They start only when the vehicle has stopped. The 7s wait is to let late-arriving vehicles stop and exchange beacons.

### 1.3 Merge Logic (handleClusterExists)

```
Current merge condition:
  canMerge = !hasCommittedOrder_ && !hasPassedIntersection_ && (cooldown passed)
  merge if: unionAddsMembers && (incomingBigger || equalSizeMerge)

  - incomingBigger: incoming cluster has MORE members than ours
  - equalSizeMerge: same size BUT union adds new members (disjoint 4+4 → merge into 8)
  - Equal-size SAME members: no merge (redundant)
```

So equal-size **disjoint** clusters (4 vs 4) **do** merge. Equal-size **overlapping** (same 4) would not add members, so no merge.

### 1.4 Key Files & Locations

| File | Relevant Code |
|------|---------------|
| `RaftAppBase.cc` | `sendDiscoveryBeacon`, `checkClusterTrigger`, `formCluster`, `handleClusterForm`, `handleClusterExists`, `mergeIntoLargerCluster` |
| `RaftAppBase.h` | `discoveredPeers_`, `clusterFormationScheduled_`, `discoveryWaitMs_`, `clusterFormationDelayMs_` |
| `UdpRaftApplication.cc` | `discoveryTimer_` handler — sends beacon only when `hasStoppedAtIntersection_` |
| `WaveRaftApplication.cc` | Same |
| `RaftWaveMessage.msg` | `DISCOVERY_BEACON=0x10`, `CLUSTER_FORM=0x11` |

---

## 2. Your Proposed System

### 2.1 Cluster Invitation Message

| Field | Type | Initial Value | After Merge |
|-------|------|---------------|-------------|
| **id** | int | myId | (sender) |
| **clusterId** | int | myId | min(vehicleIds) in cluster |
| **timestamp** | double | simTime (seconds) | min(timestamps) in cluster |

Plus: **member list** (for merge union). So full payload: `[vehicleId][clusterId][timestamp][numMembers][member0]...[memberN]`.

### 2.2 Merge Rule

- **Primary:** Older timestamp wins (vehicle that initiated first).
- **Tie-break:** Lower clusterId wins.

So cluster A (timestamp=100, id=1) beats cluster B (timestamp=100, id=2) → B joins A.

### 2.3 Flow

1. **From startup:** All vehicles send cluster invitation periodically (replaces discovery beacon).
2. **At intersection:** All vehicles stop. Wait **5 seconds** for formation/merge.
3. **During 5s:** Exchange invitations, merge using (timestamp, clusterId).
4. **After 5s:** Form RAFT cluster with merged members. Rest unchanged.

### 2.4 Benefits

- Equal-size clusters always merge (deterministic via timestamp + clusterId).
- No fixed 7s wait; 5s formation window.
- Discovery and formation overlap: messaging starts from approach.
- Simpler merge rule: no size comparison, just (timestamp, clusterId).

---

## 3. Gap Analysis & Risks

### 3.1 When Vehicles Can See Each Other

| Scenario | Current | Proposed |
|----------|---------|----------|
| Vehicles 500 m apart at t=0 | No messages until both stop (then within ~350 m) | Messages from start — may be out of range initially |
| Vehicles approach from opposite sides | See each other when both stopped (7s wait) | May see each other during approach if in range |
| Late vehicle (arrives 10 s after first) | Misses 7s window; may form separate cluster, then merge if size allows | During 5s window at stop; if in range, merge by timestamp |

**Risk:** If vehicles start very far apart, they might not receive each other’s invitations until they stop. The 5s wait at the intersection is still needed. Your proposal keeps that but shortens it (5s vs 7s).

### 3.2 Determinism of (timestamp, clusterId)

- **Timestamp:** Two vehicles can have the same simTime if they start in the same step. Use `simTime().dbl()` with sufficient precision (double).
- **Tie-break:** clusterId = min(vehicleIds). When merging, adopt the winning cluster’s (timestamp, clusterId). Deterministic.

### 3.3 Merge Direction

- Vehicle 1: clusterId=1, timestamp=100.
- Vehicle 2: clusterId=2, timestamp=100.

Vehicle 2 sees Vehicle 1: 1’s timestamp is same, clusterId lower → 1 wins → 2 joins 1.  
Vehicle 1 sees Vehicle 2: 2’s timestamp same, clusterId higher → 1 wins → 1 keeps, 2 joins when 2 processes 1’s message.

Both sides agree.

### 3.4 Startup Sending

**Current:** Discovery timer starts at init, but `sendDiscoveryBeacon()` is only called when `hasStoppedAtIntersection_` (or in COORDINATION for cluster-exists).

**Proposed:** Call send from startup. Options:

- **A:** Send cluster invitation on discovery timer from the beginning (even when not stopped).
- **B:** Start a separate “cluster invitation” timer from startup.

Recommendation: Use the same discovery timer; always send the cluster invitation (payload format changes). No need for `hasStoppedAtIntersection_` to start sending.

### 3.5 When to Form RAFT

**Current:** First vehicle to wait 7s calls `formCluster(members)`.

**Proposed:** After 5s at intersection, the vehicle with the **winning** (timestamp, clusterId) should form. But several vehicles might think they “won” (e.g. same cluster). Simplest: **any vehicle** that has waited 5s and has a non-empty merged member set forms. If two form with the same member set, the second `formCluster` will hit `clusterPhase_ != PHASE_DISCOVERY` and return (guard). So we need: **only the logical “leader” of the winning cluster forms**.

Simpler approach: **every vehicle** in DISCOVERY that has waited 5s tries to form. The member set is built from all received invitations, merged by (timestamp, clusterId). Each vehicle computes the same merged set. Whichever timer fires first calls `formCluster(mergedMembers)`. Others receive the CLUSTER_FORM/EXISTS and either merge (if they had a cluster) or join. This matches current behavior: first to fire forms; others absorb or merge.

---

## 4. Implementation Plan

### 4.1 New Message Type: CLUSTER_INVITATION (0x13)

**Payload:**
```
[vehicleId: 4B]
[clusterId: 4B]
[timestamp: 8B double]
[numMembers: 1B]
[member0: 4B] [member1: 4B] ...
```

**Sending:** From startup (or when in DISCOVERY/approaching), every `clusterInvitationInterval` (e.g. 0.3 s, same as beacon).

### 4.2 New State (RaftAppBase.h)

```cpp
int         myClusterId_;           // Initially myId_, becomes min(id) after merges
double      myClusterTimestamp_;    // Initially timeArrived_ (or first send), becomes min(ts) after merges
std::set<int> invitationMembers_;   // Union of members from winning invitations (or discoveredPeers replacement)
```

### 4.3 Logic Changes

| Location | Change |
|----------|--------|
| `sendDiscoveryBeacon` | Replace with `sendClusterInvitation()`. Payload: (myId, myClusterId_, myClusterTimestamp_, members). Send from startup (remove `hasStoppedAtIntersection_` gate for sending). |
| `handleDiscoveryBeacon` | Replace with `handleClusterInvitation(data, senderId)`. Parse (vehicleId, clusterId, timestamp, members). Merge logic below. |
| `checkClusterTrigger` | Change wait to 5 s (`clusterFormationWaitMs_` = 5000). Use `invitationMembers_` (or merged set) as members. |
| `handleClusterExists` | Merge rule: replace `incomingBigger \|\| equalSizeMerge` with `incomingWins(timestamp, clusterId)`. Adopt winning cluster’s (clusterId, timestamp). |

### 4.4 Merge Rule (Pseudocode)

```cpp
bool incomingWins(double theirTs, int theirId, double myTs, int myId) {
    if (theirTs < myTs) return true;   // older timestamp wins
    if (theirTs > myTs) return false;
    return theirId < myId;              // tie-break: lower id wins
}
```

When we adopt the incoming cluster: set `myClusterId_ = theirId`, `myClusterTimestamp_ = theirTs`, merge member sets.

### 4.5 Parameter Changes

| Parameter | Current | New |
|-----------|---------|-----|
| discoveryWaitMs | 5000 | 0 (or remove) |
| clusterFormationDelayMs | 2000 | 0 (or remove) |
| **clusterFormationWaitMs** (new) | — | 5000 |

So: single 5 s wait at intersection instead of 5s + 2s.

### 4.6 Transport Additions

- **UDP (WillemtRaftApplication):** Add case for 0x13 in `processPacket`, call `handleClusterInvitation`.
- **WAVE:** Add case for CLUSTER_INVITATION in `onWSM`, call `handleClusterInvitation`.
- **RaftWaveMessage.msg:** Add `CLUSTER_INVITATION = 19;` (or next free).

### 4.7 Backward Compatibility

Old DISCOVERY_BEACON (0x10) can be ignored or phased out. CLUSTER_FORM (0x11) stays for post-formation broadcasts. New CLUSTER_INVITATION (0x13) is the discovery/merge channel.

---

## 5. Continuous Broadcasting Until Exit (Critical Requirement)

### 5.1 Requirement

**Cluster invitations must never stop until the vehicle leaves the intersection.** Stopping only when `hasPassedIntersection_` is true. This allows:

- Clusters that couldn't merge during the initial 5 s to merge later when they finally receive each other's messages
- Late-arriving vehicles to always discover and join
- Partitioned clusters (e.g. 4+4 that missed each other initially) to eventually merge

### 5.2 Current Behavior (As-Is)

| Mechanism | Stop condition | Keeps sending after RAFT commit? |
|-----------|----------------|----------------------------------|
| **Recurring discovery timer** (every 2 s in COORDINATION) | `!hasPassedIntersection_` | ✓ Yes |
| **One-shot broadcasts** in `formCluster()` (300, 700, 1200… ms) | `!hasCommittedOrder_` | ✗ No — stops after RAFT commits |

**Conclusion:** The recurring timer keeps sending until exit. The one-shots stop at commit. So partially aligned, but the one-shots give up too early.

### 5.3 Required Behavior (Proposed)

| Phase | Send cluster invitation? | Stop when |
|-------|--------------------------|------------|
| Startup → intersection | ✓ Yes | — |
| Stopped at intersection (formation window) | ✓ Yes | — |
| RAFT running | ✓ Yes | — |
| RAFT committed (decision made) | ✓ Yes | — |
| Vehicle passed intersection | ✗ No | `hasPassedIntersection_` |

**Rule:** Send cluster invitation whenever `!hasPassedIntersection_`. Do **not** gate on `hasCommittedOrder_`.

### 5.4 Changes Needed

1. **One-shot broadcasts:** Remove `!hasCommittedOrder_` from the condition so they continue after commit (or remove one-shots and rely solely on the recurring timer).
2. **Cluster invitation (new):** Use only `!hasPassedIntersection_` as the send gate — never `hasCommittedOrder_`.
3. **Merge handling:** When we've committed, we normally don't merge (merge is for pre-commit). But we still **send** so other clusters can discover us and we can send LATE_JOIN to new vehicles. The sending logic is independent of merge logic.

---

## 6. File-by-File Change List (Updated)

| File | Changes |
|------|---------|
| `RaftShared.h` | (Optional) struct for invitation payload. |
| `RaftAppBase.h` | Add `myClusterId_`, `myClusterTimestamp_`, `clusterFormationWaitMs_`. Optional: `invitationMembers_` or reuse `discoveredPeers_`. |
| `RaftAppBase.cc` | New `sendClusterInvitation`, `handleClusterInvitation`. Modify `checkClusterTrigger` (5s, new member source). Modify `handleClusterExists` (timestamp+clusterId merge). **formCluster one-shots:** remove `!hasCommittedOrder_` so broadcasts continue until pass. **Cluster invitation send gate:** only `!hasPassedIntersection_`. |
| `UdpRaftApplication.cc` | Discovery timer: always call `sendClusterInvitation` when `!hasPassedIntersection_` (from startup, through RAFT, until exit). Add 0x13 handler. |
| `WaveRaftApplication.cc` | Same for WAVE. |
| `RaftWaveMessage.msg` | Add `CLUSTER_INVITATION = 19`. |
| `omnetpp_*.ini` | `discoveryWaitMs=0`, `clusterFormationDelayMs=0`, add `clusterFormationWaitMs=5000`. |

---

## 7. Conclusion

The proposal is sound and can be implemented with the steps above. Main effects:

1. **Timestamp + clusterId merge** — deterministic, allows equal-size merges.
2. **Single 5 s wait** — replaces 7 s discovery+formation delay.
3. **Startup sending** — earlier exchange of cluster state.
4. **Continuous broadcasting until exit** — never stop for `hasCommittedOrder_`; only stop when `hasPassedIntersection_`. Enables late merges and late joiners even after RAFT has decided.

Risks are low if we keep the 5 s formation window at the intersection. The main design choice is whether to keep DISCOVERY_BEACON for backward compatibility or fully replace it with CLUSTER_INVITATION.

### Continuous Broadcast — Achievability

**Yes, achievable.** The recurring discovery timer already sends until `hasPassedIntersection_`; the only discrepancy is the one-shot callbacks in `formCluster()` which stop at `hasCommittedOrder_`. For the new design:

- Cluster invitation is sent on the same recurring timer (or equivalent)
- Gate: `if (!hasPassedIntersection_) sendClusterInvitation()`
- No `hasCommittedOrder_` check
- One-shots (if retained): remove `!hasCommittedOrder_` from their condition
