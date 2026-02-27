#!/usr/bin/env python3
"""
Compute departPos for vehicles so they spawn pre-spaced:
  - First vehicle ~10m from its approach junction
  - Subsequent vehicles 10m apart behind it (or use length+minGap)

Reads edge lengths from SUMO net.xml, writes updated route file.

Usage:
    python3 spawn_spaced_vehicles.py [--sim-dir PATH] [--vehicles 16] [--front-dist 10] [--spacing 10]

For intersection_16 only (as requested).
"""
import argparse
import xml.etree.ElementTree as ET
from pathlib import Path


def parse_args():
    p = argparse.ArgumentParser(description="Spawn vehicles pre-spaced along routes")
    p.add_argument("--sim-dir", default="simulations/intersection_16", help="Simulation directory")
    p.add_argument("--vehicles", type=int, default=16)
    p.add_argument("--front-dist", type=float, default=10.0,
                   help="Distance of front vehicle from junction (m)")
    p.add_argument("--spacing", type=float, default=10.0,
                   help="Spacing between vehicles (m) - use length+minGap or larger")
    return p.parse_args()


def get_edge_lengths(net_path):
    """Parse net.xml and return {edge_id: length}."""
    tree = ET.parse(net_path)
    root = tree.getroot()
    lengths = {}
    for edge in root.findall("edge"):
        if edge.get("function") == "internal":
            continue
        eid = edge.get("id", "")
        lanes = edge.findall("lane")
        if lanes:
            lengths[eid] = float(lanes[0].get("length", "100"))
    return lengths


def route_length(edges, lengths):
    """Total length of route in meters."""
    return sum(lengths.get(e, 0) for e in edges)


def position_to_route_pos(edges, lengths, approach_edge, dist_from_junction):
    """
    Given distance from junction along the approach (positive = back from junction),
    return route position (meters from route start).
    Junction is at end of approach edge.
    """
    junction_pos = 0
    for e in edges:
        junction_pos += lengths.get(e, 0)
        if e == approach_edge:
            break
    return junction_pos - dist_from_junction


# Route definitions and approach edges (last edge before central junction)
# Approach edge = edge vehicles stop on; junction is at its end
ROUTES = {
    "rW": {
        "edges": ["-34894202#14", "-34894202#13", "-34894202#10", "-34894202#8"],
        "approach": "-34894202#13",  # junction at end of this edge
    },
    "rS": {
        "edges": ["34894202#7", "34894202#8", "34894202#10", "34894202#13", "34894202#14"],
        "approach": "34894202#10",
    },
    "rE": {
        "edges": ["36905242#8", "36905242#11", "-37551802#8", "-37551802#6"],
        "approach": "36905242#11",
    },
    "rN": {
        "edges": ["37551802#6", "37551802#8", "-36905242#11", "-36905242#8"],
        "approach": "37551802#8",
    },
}

# 16 vehicles: veh0-3→rW, veh4-7→rS, veh8-11→rE, veh12-15→rN
VEHICLE_ROUTES = {
    0: "rW", 1: "rW", 2: "rW", 3: "rW",
    4: "rS", 5: "rS", 6: "rS", 7: "rS",
    8: "rE", 9: "rE", 10: "rE", 11: "rE",
    12: "rN", 13: "rN", 14: "rN", 15: "rN",
}


def main():
    args = parse_args()
    sim_dir = Path(args.sim_dir)
    net_path = sim_dir / "osm.net.xml"
    rou_path = sim_dir / "intersection.rou.xml"

    if not net_path.exists():
        print(f"Error: {net_path} not found")
        return 1
    if not rou_path.exists():
        print(f"Error: {rou_path} not found")
        return 1

    lengths = get_edge_lengths(net_path)

    # Compute departPos per vehicle
    # For each direction: front at front_dist from junction, others spaced behind
    depart_positions = {}
    for vid in range(args.vehicles):
        route_id = VEHICLE_ROUTES.get(vid, "rW")
        route_info = ROUTES[route_id]
        edges = route_info["edges"]
        approach = route_info["approach"]

        # Position index within this direction (0=front, 1=second, ...)
        vehicles_per_side = args.vehicles // 4
        pos_in_lane = vid % vehicles_per_side

        # Front vehicle: front_dist from junction
        # Each additional: + spacing behind
        dist_from_junction = args.front_dist + pos_in_lane * args.spacing
        route_pos = position_to_route_pos(edges, lengths, approach, dist_from_junction)

        # Clamp to valid route (>= 0, < total length)
        total_len = route_length(edges, lengths)
        route_pos = max(0, min(route_pos, total_len - 0.1))
        depart_positions[vid] = round(route_pos, 2)

    # Read existing route file
    tree = ET.parse(rou_path)
    root = tree.getroot()

    # Find vehicle elements and update departPos
    vehicles = root.findall("vehicle")
    for v in vehicles:
        vid_str = v.get("id", "")
        if vid_str.startswith("veh"):
            try:
                vid = int(vid_str[3:])
            except ValueError:
                continue
            if vid in depart_positions:
                v.set("departPos", str(depart_positions[vid]))

    # Write back
    tree.write(rou_path, encoding="unicode", default_namespace="", method="xml")
    # Fix XML declaration
    content = rou_path.read_text(encoding="utf-8")
    if not content.strip().startswith("<?xml"):
        content = '<?xml version="1.0" encoding="UTF-8"?>\n' + content
    rou_path.write_text(content, encoding="utf-8")

    print(f"Updated {rou_path}")
    for vid in range(args.vehicles):
        r = VEHICLE_ROUTES.get(vid, "rW")
        print(f"  veh{vid} ({r}): departPos={depart_positions[vid]:.2f}m")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
