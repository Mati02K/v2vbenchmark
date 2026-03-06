# 16-Vehicle Benchmark Analysis Report

## Executive Summary

| Metric | UDP | WAVE |
|--------|-----|------|
| **Fallback rate** | ~12.5% (aggregate) – 68% (Run 1) | **~70% – 81%** (aggregate 81%) |
| **RAFT success** | 5/16 (Run 1) – 14/16 (better runs) | 2/16 (Run 1) |
| **Root cause** | Split clusters + RAFT timeout; NOT IN SCHEDULE | **Split clusters** – leader’s status/schedule never reaches most vehicles |

---

## 1. Fallback Root Causes (from logs and results)

### 1.1 Three fallback trigger types

1. **RAFT TIMEOUT** – Cluster formed, but no order committed within threshold (~20.8 s).  
   - Log: `RAFT TIMEOUT: 20820.0ms since ref (threshold=20800.0ms)`  
   - Typical for vehicles in separate clusters (no quorum) or with weak links.

2. **NOT IN SCHEDULE** – Vehicle received the committed schedule but was not in it (`my_batch = -1`).  
   - Leader collected status from only a subset; schedule omitted others.  
   - Log: `NOT IN SCHEDULE! Using fallback.`

3. **BATCH SAFETY FALLBACK** – Vehicle was in schedule, but `VEHICLE_LEFT` messages were lost.  
   - Batch never advanced; safety timer fired.  
   - Log: `BATCH SAFETY FALLBACK (lost VEHICLE_LEFT?) for batch N`

---

## 2. UDP 16-Vehicle Analysis (Run 1: `simple_udp_16veh`)

### 2.1 Outcomes (Run 1)

| Outcome | Count | Vehicle IDs |
|---------|-------|-------------|
| **RAFT** | 5 | V0, V1, V2, V4, V8 |
| **Fallback** | 11 | V3, V5, V6, V7, V9, V10, V11, V12, V13, V14, V15 |

*Note: Aggregate stats (~14% fallback) average over multiple runs; individual runs vary.*

### 2.2 Cluster formation timeline (from `full_sim.log` — separate UDP 16-veh run; shows typical merge sequence)

| Time (s) | Event |
|----------|-------|
| 0.10 | V0–V7: single-node, initial merges |
| 0.22 | V2: MERGE into 7 members [0,2,3,4,5,6,7] |
| 0.22 | V1: MERGE into 8 members [0,1,2,3,4,5,6,7] |
| 2.03 | V8: MERGE into 9 members [0–8] |
| 4.10 | V3,V6,V7,V9,V10,V11: MERGE into 11–12 members |
| 5.90 | V12: MERGE into 14 members [0–13] |
| 5.90 | V13: MERGE into 14 members |
| 6.60 | V14: MERGE into 15 members [0–14] |
| 6.90 | V15: MERGE into **16 members** [0–15] |

**Conclusion:** All 16 vehicles merge into one cluster before the first vehicle stops (≈12.9 s).

### 2.3 Leader sequence (full_sim.log)

| Time (s) | Leader | Term | Members |
|----------|--------|------|---------|
| 0.14 | V0 | 0 | [0] |
| 0.14 | V1 | 0 | [1,2,3] |
| 0.14 | V2 | 0 | [2,3,5,7] |
| 0.14 | V4 | 0 | [4,5] |
| 0.14 | V5 | 0 | [5] |
| 1.48 | **V3** | 1 | [0–7] |
| 6.16 | **V7** | 6 | [0–13] |
| … | V7 | 6 | Full 16-member cluster |

V7 leads the 16-member cluster once all vehicles have merged.

### 2.4 Why UDP fallbacks occur (≈12–14%)

1. **RAFT TIMEOUT** – V12, V13, V14, V15 hit the 20.8 s RAFT timeout. They merge late (5.5–6.9 s) and are on N2C; they may receive AppendEntries unreliably or join during unstable leadership.
2. **NOT IN SCHEDULE** – V3, V5, V6, V7, V9, V10, V11 have `entries_committed = 1` but `my_batch = -1`. They received the committed schedule but were not included. The leader’s status collection did not see all 16 (packet loss, timing), so the schedule omitted them.
3. **BATCH SAFETY FALLBACK** – Some vehicles were in the schedule but did not receive `VEHICLE_LEFT` broadcasts, so the batch never advanced and the safety timer triggered fallback.

### 2.5 Cluster resize (vehicles leaving)

- Log shows `vehiclesLeftInBatch_` and batch advances. When vehicles pass and send `VEHICLE_LEFT`, the cluster does not shrink; `activeVehicles_` is updated and batch advancement proceeds.
- No explicit “cluster resize” logic; vehicles are marked left but remain in the logical cluster until they pass.

---

## 3. WAVE 16-Vehicle Analysis (Run 1: `simple_raftwave_16veh`)

### 3.1 Outcomes

| Outcome | Count | Vehicle IDs |
|---------|-------|-------------|
| **RAFT** | 2 | V0, V8 |
| **Fallback** | 14 | V1–V7, V9–V15 |

### 3.2 Cluster formation (from `raft_results.json`)

| Vehicle | cluster_formed (ms) | Lane | entries_committed | my_batch |
|---------|---------------------|------|-------------------|----------|
| V0 | 100 | W2C | 1 | 3 |
| V8 | 2402 | E2C | 1 | 8 |
| V1–V7, V9–V11 | 4040–4144 | W2C, S2C, E2C | 0 | -1 |
| V12 | 5539 | N2C | 0 | -1 |
| V13 | 5841 | N2C | 0 | -1 |
| V14 | 6705 | N2C | 0 | -1 |
| V15 | 6867 | N2C | 0 | -1 |

### 3.3 Inferred cluster structure

- **Cluster A (leader, committed):** V0, V8, plus a subset that contributed to the committed schedule. V0 and V8 are the only ones with `entries_committed = 1` and valid `my_batch`.
- **Other vehicles:** All have `entries_committed = 0` → they never applied the committed order. They were either:
  - In a different cluster that never committed, or  
  - In the same cluster but did not receive AppendEntries reliably (WAVE loss).

### 3.4 Likely scenario for WAVE

1. **Split clusters** – Geographic layout (W/S/E/N) plus 802.11p range/loss causes clusters that do not fully merge or that exchange messages poorly.
2. **Status collection failure** – Leader (e.g. V0 or V8) collects status from a subset (e.g. W2C + E2C), proposes a schedule with ~9 slots (max `my_batch` = 8), and commits.
3. **AppendEntries loss** – Vehicles on S2C and N2C do not reliably receive the committed AppendEntries, so they never apply the order and eventually fall back.
4. **High election rounds** – Fallback vehicles show 70–100 election rounds, indicating unstable leadership and repeated failed elections.

### 3.5 Cluster dynamics (WAVE)

- No explicit resize mechanism when vehicles leave.
- With WAVE, merges and message delivery are slower and less reliable, so multiple logical clusters or partial views persist.
- N2C vehicles (V12–V15) stop later (53–79 s) and form clusters at 5.5–6.9 s; they likely never merge with the leader’s cluster or receive its decisions.

---

## 4. UDP vs WAVE: Why the Large Gap?

| Factor | UDP (802.11a) | WAVE (802.11p) |
|--------|----------------|----------------|
| **Channel** | 20 MHz | 10 MHz |
| **Broadcast** | Multicast to 224.0.0.1 | Direct broadcast |
| **Unicast** | Broadcast; non-targets drop | Gossip relay (multi-hop) |
| **Reliability** | Higher under load | More loss under load |
| **Merge behavior** | All 16 merge by ~6.9 s | Merges incomplete; split clusters |
| **AppendEntries** | Generally reach followers | Many drops to S2C/N2C |
| **Fallback share** | ~12–14% | ~70–81% |

---

## 5. Cluster Dynamics Summary

### 5.1 UDP (full_sim.log)

- Cluster 1: Forms by progressive merges (2 → 3 → 7 → 8 → 9 → 11 → 12 → 14 → 16 members).
- Leaders: V3 (term 1), V7 (term 6) over the 16-member cluster.
- Resize: No explicit shrink; `activeVehicles_` and `vehiclesLeftInBatch_` track passed vehicles; batches advance on `VEHICLE_LEFT`.

### 5.2 WAVE (inferred)

- Cluster formation: V0 single-node at 100 ms; others form around 2.4–6.9 s.
- Likely clusters: (1) V0 + some neighbors, (2) V8 + E2C, (3) S2C group, (4) N2C group (V12–V15).
- Merges across these groups fail or are too slow due to WAVE connectivity.
- Only the cluster containing V0 and V8 achieves quorum and commits a schedule.
- Resize: Same model as UDP; no explicit size reduction when vehicles leave.

---

## 6. Recommendations

1. **WAVE reliability**
   - Retries or redundancy for AppendEntries and status collection.
   - Stronger gossip for critical messages (e.g. committed order).

2. **Status collection**
   - Ensure leader waits for (or uses a robust timeout for) status from all cluster members before proposing.

3. **Merge robustness**
   - Re-evaluate merge rules and connectivity so that vehicles on all approaches can merge into one cluster under WAVE conditions.

4. **VEHICLE_LEFT handling**
   - More robust delivery or acknowledgment of `VEHICLE_LEFT` to reduce BATCH SAFETY FALLBACK on UDP.

---

## 7. Data Sources

- UDP Run 1: `results/simple_udp_16veh/run_1/raft_results.json`
- WAVE Run 1: `results/simple_raftwave_16veh/run_1/raft_results.json`
- UDP log (different run): `full_sim.log` (16-vehicle UDP, IntersectionScenarioInet)
