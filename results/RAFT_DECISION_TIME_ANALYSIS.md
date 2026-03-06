# RAFT Decision Time: Root-Cause Analysis

## Summary of Findings

| Issue | Root Cause |
|-------|------------|
| **WAVE 16 = 32 ms** | plot_comparison.py loads ALL runs (1-10); many runs have 1 proposer with 2-20 ms; mean of 10 = 32 |
| **UDP 8 < UDP 4** | Run 1 of UDP 8 had sum=20 (one proposer); runs 2-3 had 1600-1640. Lucky run pulls mean down |
| **Metric inconsistency** | Sum across ALL proposers mixes different clusters; not per-decision latency |
| **Truncated JSON** | WAVE 16: not all 16 vehicles stop before 300s; 13-15 write; file never closes (no `]`) |

---

## 1. Why WAVE 16 Shows 32 ms

### Data source mismatch
- **aggregate_stats.json** uses runs 1–3 only → mean = **93.3 ms**
- **plot_comparison.py** loads run_1..run_N until none exist → uses **all 10 runs** → mean = **32.0 ms**

### Per-run sums (WAVE 16, 10 runs)
```
Run 1: sum_rd=40   (proposers: V0=20, V8=20)
Run 2: sum_rd=40   (proposers: V0=40)
Run 3: sum_rd=200  (proposers: V0=20, V11=180)
Run 4: sum_rd=3    (proposers: V8=3)
Run 5: sum_rd=2    (proposers: V0=2)
Run 6: sum_rd=5    (proposers: V8=4.7)
Run 7: sum_rd=5    (proposers: V0=2.7, V8=2.5)
Run 8: sum_rd=20   (proposers: V0=19.5)
Run 9: sum_rd=5    (proposers: V0=4.6)
Run 10: sum_rd=1   (proposers: V0=0.6)

Mean over 10 runs = 32.0 ms
Mean over runs 1–3 only = 93.3 ms
```

In many runs, consensus is fast (2–20 ms) because WAVE has high delivery ratio and/or few concurrent proposers.

---

## 2. Why UDP 8 < UDP 4

### UDP 4 (runs 1–3)
| Run | sum_rd | Proposers |
|-----|--------|-----------|
| 1   | 1640   | V0=20, V3=800, V2=820 |
| 2   | 2430   | V0=20, V1=810, V3=800, V2=800 |
| 3   | 800    | V0=800 |
| **Mean** | **1623** | |

### UDP 8 (runs 1–3)
| Run | sum_rd | Proposers |
|-----|--------|-----------|
| 1   | 20     | V3=20 only |
| 2   | 1600   | V0=800, V4=800 |
| 3   | 1640   | V3=820, V0=20, V4=800 |
| **Mean** | **1087** | |

Run 1 of UDP 8 has a single proposer with 20 ms consensus; runs 2–3 are high. The single low run pulls the mean down.

---

## 3. Truncated JSON (WAVE 16)

### What happens
- `RaftMetrics` closes the JSON when `vehiclesCompleted_ >= totalVehiclesStatic_`.
- `totalVehiclesStatic_` = 16.
- Only vehicles with `hasStoppedAtIntersection_` write their record.
- At 300 s, some vehicles never reach/stop at the intersection → only 13–15 write.
- `vehiclesCompleted_` never reaches 16 → `closeResultsFile()` never runs → no final `]`.
- Result: malformed JSON ending in `}` instead of `]`.

### Fix
- Close when the simulation ends (or after a timeout), even if not all vehicles wrote.
- Alternatively, set `totalVehiclesStatic_` to the number of vehicles that actually write.

---

## 4. Why the Current Metric Is Misleading

### Current definition
RAFT decision time = sum over all proposers of (status collection + sum(commit − propose))  
Each vehicle with `entries_proposed > 0` contributes its `raft_decision_time`; we sum across all such vehicles.

### Problems
1. **Multiple clusters**: V0, V3, V2 can be leaders of *different* clusters. We add three separate consensus costs.
2. **Different batches**: Within one cluster, multiple leaders over time (e.g. re-elections) each contribute. We sum all of them.
3. **Interpretation**: The result is “total system consensus cost,” not “per-decision latency.”

### What you likely want
- **Per-decision latency**: Time from propose to commit for a single log entry.
- **Per-run metric**: Either the dominant cluster’s decision time, or max/mean across clusters.

---

## 5. Recommendations

### A. Consistent run set
- Use the same runs for aggregation and plotting (e.g. runs 1–3).
- In `plot_comparison.py`, stop at `num_runs` instead of loading until no file exists.

### B. RAFT decision time metric
Pick one:

1. **Per-decision**: Max or mean of (commit − propose) per log entry, per cluster.
2. **Dominant cluster only**: Only count the cluster that commits the final ordering (e.g. the one with the most vehicles).
3. **Per batch**: Report time for one batch only (status collection + first commit), not summed over all batches.

### C. Truncated JSON
- Add an OMNeT++ `finish()` hook (or module that runs last) that calls `RaftMetrics::closeResultsFile()` regardless of `vehiclesCompleted_`.
- Or track “vehicles that will write” and close when the simulation ends.

### D. Longer sim for 16 vehicles
- With 300 s, some vehicles never stop. Consider `sim-time-limit = 600s` for the 16-vehicle scenario, or a scenario where all 16 reliably stop.

### E. Variance and outliers
- Report median in addition to mean, or drop the min/max run to reduce impact of extreme runs.

---

## 6. Quick Checks You Can Run

```bash
# Per-run RAFT decision sums
for proto in udp raftwave; do
  for vc in 4 8 16; do
    echo "=== ${proto} ${vc}veh ==="
    for r in 1 2 3; do
      python3 -c "
import json
d=json.load(open('results/simple_${proto}_${vc}veh/run_$r/raft_results.json'))
raft=[v for v in d if v.get('coordination_method')!='fallback']
rd=[v['durations_ms'].get('raft_decision_time',0) for v in raft if v['durations_ms'].get('raft_decision_time',0)>0]
print(f'  Run $r: sum={sum(rd):.0f}')
" 2>/dev/null || echo "  Run $r: FAILED"
    done
  done
done
```
