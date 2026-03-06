# Simple Intersection Benchmark Report

**Configuration:** 2 iterations each | UDP + WAVE | 4, 8, 16 vehicles  
**Date:** Generated from `run_simple_benchmark.sh 2`  
**Scenarios:** `simple_intersection_4`, `simple_intersection_8`, `simple_intersection`

---

## Summary Table

| Scenario      | Vehicles | RAFT (Run 1 / Run 2) | Fallback | Fallback Rate | Total Intersection | Avg Wait | Avg Transit |
|---------------|----------|----------------------|----------|---------------|-------------------|----------|-------------|
| UDP 4veh      | 4        | 4 / 4                | 0 / 0    | **0%**        | 40.5 s            | 37.9 s   | 885 ms      |
| WAVE 4veh     | 4        | 4 / 4                | 0 / 0    | **0%**        | 37.5 s            | 36.3 s   | 881 ms      |
| UDP 8veh      | 8        | 8 / 8                | 0 / 0    | **0%**        | 44.4 s            | 38.8 s   | 962 ms      |
| WAVE 8veh     | 8        | 8 / 8                | 0 / 0    | **0%**        | 48.7 s            | 41.7 s   | 805 ms      |
| UDP 16veh     | 16       | 9 / 14               | 7 / 0    | **21.9%**     | 76.8 s            | 48.6 s   | 795 ms      |
| WAVE 16veh    | 14*      | 14 / 14              | 0 / 0    | **0%**        | 62.2 s            | 41.5 s   | 741 ms      |

\* WAVE 16veh: 2 vehicles did not pass before 300 s sim limit, so metrics show 14.

---

## Per-Scenario Details

### 4 Vehicles
- **UDP:** 100% RAFT, 0% fallback in both runs.
- **WAVE:** 100% RAFT, 0% fallback in both runs.

### 8 Vehicles
- **UDP:** 100% RAFT, 0% fallback in both runs.
- **WAVE:** 100% RAFT, 0% fallback in both runs.

### 16 Vehicles
- **UDP:** Run 1: 9 RAFT, 7 fallback (44% fallback). Run 2: 14 RAFT, 0 fallback. Higher variance from cluster formation and packet loss.
- **WAVE:** 100% RAFT in both runs among vehicles that passed. Improved vs. prior 70–81% fallback due to 1.5 s VEHICLE_LEFT timeout.

---

## Data Sources

- `results/simple_udp_4veh/run_1`, `run_2/raft_results.json`
- `results/simple_raftwave_4veh/run_1`, `run_2/raft_results.json`
- `results/simple_udp_8veh/run_1`, `run_2/raft_results.json`
- `results/simple_raftwave_8veh/run_1`, `run_2/raft_results.json`
- `results/simple_udp_16veh/run_1`, `run_2/raft_results.json`
- `results/simple_raftwave_16veh/run_1`, `run_2/raft_results.json`
