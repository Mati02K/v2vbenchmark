# RAFT Early Start + Dynamic Membership — Implementation Proposal

## Summary of Proposed Changes

| Current | Proposed |
|---------|----------|
| RAFT starts only after 5s stopped at intersection | RAFT starts at vehicle initialization |
| Discovery → 5s wait → formCluster → RAFT | Discovery + merge during approach; RAFT runs throughout |
| 5s stop = formation window | 5s stop = aggressive merge window (10ms + jitter invitations) |
| Late joiners: LATE_JOIN_ORDER (no RAFT add) | All joiners: raft_add_node via consensus; quorum auto-adjusts |
| Vehicles leaving: VEHICLE_LEFT (no raft_remove) | Vehicles leaving: DEMOTE_NODE + REMOVE_NODE via consensus |

---

## 1. RAFT Start at Initialization

**Change:** Create RAFT in `initialize()` (stage 2) or `startApplication()` instead of `formCluster()`.

**Implementation:**
- Each vehicle calls `raft_new()`, `raft_add_node(self)` at init → single-node cluster.
- `raftPeriodicTimer_` starts immediately (every 20 ms).
- Merge logic (during approach) uses `mergeIntoLargerCluster` as today: loser dissolves, re-forms with `formCluster(union)`.

**Concern:** Single-node RAFT will elect self as leader immediately. Before intersection, we must avoid proposing status/pass order. Gate status request and pass order on `hasStoppedAtIntersection_` and a “ready” condition (e.g. 5s elapsed or cluster stable).

---

## 2. Discovery + Merge During Approach

**Keep:** CLUSTER_INVITATION, timestamp+clusterId merge rule.  
**Change:** No 5s wait before forming. Merge can happen any time.

**Flow:**
- From init: broadcast CLUSTER_INVITATION (e.g. every 300ms + jitter).
- On receipt: update `discoveredPeers_`, adopt (clusterId, timestamp) if incoming wins.
- **Merge:** When receiving a “winning” cluster (older timestamp or tie-break lower clusterId), call `mergeIntoLargerCluster(union)`.
- Each vehicle may start as single-node; as they discover each other, the “losing” cluster re-forms with the union. Same logic as today.

**Implementation:** Remove the 5s gate from `checkClusterTrigger()`. Instead, call a formation/merge check on every discovery timer tick when we have new peers. The first “stable” union forms the cluster. Edge case: two vehicles might form at the same sim time; timestamp+clusterId should still give a consistent winner.

---

## 3. 5s Stop = Aggressive Merge Window

**Change:** When stopped at intersection, send CLUSTER_INVITATION every **10 ms + random jitter** instead of ~300ms.

**Implementation:**
- New state or flag: `hasStoppedAtIntersection_`.
- While stopped and in PHASE_DISCOVERY or early COORDINATION: discovery timer interval = `10 + uniform(0, 5)` ms.
- After 5s: switch to normal interval (or stop if merged) and start status collection.
- Goal: any remaining split clusters merge within 5s.

**Concern:** 10ms invitations × N vehicles = high message rate. For 16 vehicles that’s ~1.6 inv/s/vehicle → ~25 inv/s total. May increase channel load; worth measuring.

---

## 4. Dynamic RAFT Membership (Add Node)

**Change:** New vehicles join via RAFT membership change, not LATE_JOIN_ORDER.

**willemt/raft API:**
- `raft_recv_entry(server, &entry, &response)` with `entry.type = RAFT_LOGTYPE_ADD_NODE`.
- Entry data must encode `node_id`. Library uses `log_get_node_id` callback to parse it.
- **Requirement:** Implement `log_get_node_id` in RaftAppBase (currently not set).

**Data format for ADD_NODE:**
```c
// entry.data.buf = pointer to [vehicleId: 4 bytes]
// raft_node_id_t = vehicleId + 1  (same as getNodeIdFromVehicleId)
```

**Flow:**
1. Leader detects new member (via CLUSTER_INVITATION / CLUSTER_EXISTS with new vehicle).
2. Leader proposes ADD_NODE entry: `raft_recv_entry(entry.type=RAFT_LOGTYPE_ADD_NODE, data=[vehicleId])`.
3. Entry is replicated and committed.
4. On commit, `applylog` runs; library adds node via `raft_offer_log` → `raft_add_node_internal`.
5. Quorum becomes majority of new size automatically.

**Implementation checklist:**
- Add `log_get_node_id` callback. For ADD_NODE / ADD_NONVOTING_NODE / DEMOTE_NODE / REMOVE_NODE, parse vehicleId from `entry.data.buf` and return `vehicleId + 1`.
- Add `proposeAddRaftNode(int vehicleId)` in RaftAppBase, called when leader sees a new member. Guard with “not already in RAFT”.
- Handle `RAFT_ERR_ONE_VOTING_CHANGE_ONLY`: only one voting change at a time. Queue or retry.

**Concern:** The new node does not have the log. Standard Raft adds non-voting first (RAFT_LOGTYPE_ADD_NONVOTING_NODE) for catch-up, then promotes (RAFT_LOGTYPE_ADD_NODE). We can add voting directly if the log is small; otherwise prefer ADD_NONVOTING_NODE → wait for catch-up → ADD_NODE.

---

## 5. Dynamic RAFT Membership (Remove Node)

**Change:** When a vehicle exits (VEHICLE_LEFT), remove it from RAFT via consensus.

**willemt/raft API:**
- Two-step: `RAFT_LOGTYPE_DEMOTE_NODE` (strip vote) then `RAFT_LOGTYPE_REMOVE_NODE` (mark inactive / remove).
- Library applies these in applylog; quorum reflects active voting nodes.

**Flow:**
1. On VEHICLE_LEFT (or exit detection): leader proposes DEMOTE_NODE for that vehicle.
2. After commit: propose REMOVE_NODE.
3. Quorum shrinks automatically.

**Implementation:**
- Add `proposeRemoveRaftNode(int vehicleId)`.
- Call from VEHICLE_LEFT handling (leader side). Today we propose VEHICLE_LEFT as a normal log entry; we need an extra DEMOTE+REMOVE sequence.
- **Ordering:** Demote before remove. Library expects this order.

**Concern:** If many vehicles leave quickly, we may hit RAFT_ERR_ONE_VOTING_CHANGE_ONLY. Need a queue or pacing.

---

## 6. Replace LATE_JOIN_ORDER for Post-Commit Joiners

**Current:** After commit, new vehicles get LATE_JOIN_ORDER with an extended schedule; they never join RAFT.

**Proposed:** All new vehicles are added via ADD_NODE, including post-commit joiners.

**Implications:**
- Post-commit joiner: add via ADD_NODE → quorum increases.
- Pass order: already committed. We need a way to assign the new vehicle a batch.
- Options:
  - **A:** Propose a new PASS_ORDER (or ADD_BATCH) entry extending the schedule. Goes through consensus. Clean.
  - **B:** Keep a hybrid: add to RAFT for quorum, but still send a LATE_JOIN_ORDER-like message for batch assignment. Simpler but mixed semantics.

**Recommendation:** Option A. Add a new entry type (e.g. PASS_ORDER_EXTENSION or re-use PASS_ORDER with extended schedule). Leader proposes it after ADD_NODE commits. All nodes apply the extended schedule.

---

## 7. Implementation Order

| Phase | Task | Dependencies |
|-------|------|--------------|
| 1 | RAFT start at init (single-node) | None |
| 2 | Remove 5s gate from formation; merge during approach | Phase 1 |
| 3 | 10ms + jitter invitations when stopped | Phase 2 |
| 4 | Implement log_get_node_id | None |
| 5 | proposeAddRaftNode + proposeRemoveRaftNode | Phase 4, merge logic |
| 6 | Trigger add on new member discovery; trigger remove on VEHICLE_LEFT | Phase 5 |
| 7 | Replace LATE_JOIN_ORDER with ADD_NODE + schedule extension | Phase 5, 6 |

---

## 8. Concerns and Open Points

1. **Single-node leader:** With 1-node RAFT, each vehicle is leader. Status request / pass order must be gated until we have a real multi-node cluster and are “ready” (e.g. stopped 5s). Otherwise we get bogus decisions.

2. **Merge races:** Two clusters merging concurrently; need to ensure timestamp+clusterId gives a well-defined order. Current logic should be fine.

3. **RAFT_ERR_ONE_VOTING_CHANGE_ONLY:** Raft allows at most one voting change in-flight. With many join/leave events, we must serialize or queue. Possible extra layer: “pending membership changes” queue, process one at a time.

4. **log_get_node_id:** Must be implemented for ADD_NODE, ADD_NONVOTING_NODE, DEMOTE_NODE, REMOVE_NODE. For NORMAL and our custom types, return a sentinel or handle safely.

5. **Catch-up for new nodes:** If we add with ADD_NODE directly, the new node has an empty log. It must receive AppendEntries to catch up. The library’s ADD_NONVOTING_NODE path supports this; we should confirm behavior for our scenario.

6. **10ms load:** 10ms invitations may stress the channel. Consider 50ms or 20ms if 10ms is too aggressive.

7. **Backward compatibility:** This is a large behavioral change. Keep old path behind a config flag (e.g. `useEarlyRaftStart`) during transition.

---

## 9. File-Level Changes (Summary)

| File | Changes |
|------|---------|
| `RaftAppBase.h` | New: `log_get_node_id` static, `proposeAddRaftNode`, `proposeRemoveRaftNode`; flag for “ready to propose” |
| `RaftAppBase.cc` | RAFT init in new `initRaftSingleNode()`; `log_get_node_id` impl; add/remove proposal; merge/formation logic; gate status/pass on “ready” |
| `UdpRaftApplication.cc` / `WaveRaftApplication.cc` | Start raftPeriodicTimer at init; call initRaftSingleNode |
| `applylog` in RaftAppBase | Handle ADD_NODE / REMOVE_NODE (if needed beyond library); handle schedule extension entry |
| `omnetpp_*.ini` | New params: `invitationIntervalStoppedMs`, `useEarlyRaftStart` |

---

## 10. Quorum Behavior (Library)

The willemt/raft library computes quorum from active voting nodes. Adding nodes (ADD_NODE) and removing/demoting (DEMOTE_NODE, REMOVE_NODE) updates the node set. We only need to correctly propose these entries; quorum adjustment is handled inside the library.
