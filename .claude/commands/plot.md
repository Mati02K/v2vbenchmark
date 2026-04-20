Regenerate benchmark comparison plots.

Arguments: $ARGUMENTS

Parse `$ARGUMENTS`:
- Contains `compare` → run `--compare-modes`
- Contains `allVehicles` → mode = `allVehicles`
- Contains `nopriority` → append `_nopriority` to mode
- Otherwise → mode = `laneLeaders`

Run directly (no agent needed):

```bash
cd /home/mathesh/omnetpp-workspace/benchmark
python3 plot_comparison.py --simple --mode <mode>
# or for compare:
python3 plot_comparison.py --simple --compare-modes
```

Report which PNGs were saved or any error output.
