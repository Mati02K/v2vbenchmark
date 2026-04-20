Check and summarise benchmark results across all scenarios.

Arguments: $ARGUMENTS

Filter by args if provided (e.g. `wave 16`, `allVehicles`, `nopriority`). Show all if empty.

Run this Python snippet directly with the Bash tool:

```python
import json, os, glob
base = '/home/mathesh/omnetpp-workspace/benchmark/results'
folders = sorted(f for f in os.listdir(base) if os.path.isdir(os.path.join(base, f)))

# Apply filter from args if any
filter_args = "$ARGUMENTS".split()

for folder in folders:
    if filter_args and not any(a in folder for a in filter_args):
        continue
    fpath = os.path.join(base, folder)
    runs = sorted(glob.glob(f'{fpath}/run_*/raft_results.json'))
    total, raft, fallback = 0, 0, 0
    wait_times = []
    corrupt = []
    for rj in runs:
        try:
            data = json.load(open(rj))
            total += len(data)
            raft  += sum(1 for v in data if v.get('coordination_method') != 'fallback')
            fallback += sum(1 for v in data if v.get('coordination_method') == 'fallback')
            for v in data:
                wt = v['durations_ms'].get('total_wait_time', 0)
                if wt > 0: wait_times.append(wt)
        except: corrupt.append(os.path.basename(os.path.dirname(rj)))
    fb_pct = fallback / total * 100 if total else 0
    avg_wt = sum(wait_times) / len(wait_times) if wait_times else 0
    status = 'CLEAN' if fallback == 0 else f'FALLBACKS:{fallback}'
    print(f'{folder:45s}  runs={len(runs):2d}  raft={raft:3d}/{total:3d} ({fb_pct:.0f}% fb)  wait={avg_wt:.0f}ms  [{status}]')
    for c in corrupt: print(f'  !! CORRUPT: {c}')
```

Print the table. No agent needed.
