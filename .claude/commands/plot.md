Regenerate all benchmark plots and channel utilization outputs.

Arguments: $ARGUMENTS (ignored — plots always regenerate from whatever results exist)

Run directly:

```bash
cd /home/mathesh/omnetpp-workspace/benchmark
python3 plot_comparison.py
python3 plot_heatmap.py
python3 plot_animation.py
```

`plot_comparison.py` — scans all result dirs, generates per-transport PNGs:
- WAVE: laneLeaders, allVehicles, multirounds lines
- UDP: allVehicles line only
- `priority.png` — 3 bars: priority vehicle / normal vehicles (priority runs) / no-priority baseline

`plot_heatmap.py` — channel utilization from CSV files, outputs to top-level `results/`:
- `channel_utilization_heatmap_wave.png` — 3×3 grid (modes × vehicle counts), fixed colour scale
- `channel_utilization_timeseries_wave.png` — mean utilization over time per mode

`plot_animation.py` — generates two GIFs (priority dirs only, 4→8→16→24→32 progression):
- `results/channel_utilization_animation_laneleaders.gif`
- `results/channel_utilization_animation_allvehicles.gif`

Report which files were saved or any error output.
