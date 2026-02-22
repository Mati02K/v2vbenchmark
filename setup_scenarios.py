#!/usr/bin/env python3
"""
Setup scenarios for RAFT V2V intersection benchmark.
Auto-detects intersection edges from any SUMO/OSM network file.

Usage:
    python3 setup_scenarios.py [--network PATH] [--junction JUNCTION_ID] [--vehicles 4,8,16,32]

Defaults:
    --network   simulations/raft/osm.net.xml
    --junction  cluster_2378555322_2378555323_2378555324_2378555325_91053547
    --vehicles  4,8,16,32
"""
import os
import sys
import argparse
import xml.etree.ElementTree as ET
from pathlib import Path


def parse_args():
    parser = argparse.ArgumentParser(description="Setup RAFT benchmark scenarios")
    parser.add_argument("--network", default="simulations/raft/osm.net.xml",
                        help="Path to SUMO network file")
    parser.add_argument("--junction",
                        default="cluster_2378555322_2378555323_2378555324_2378555325_91053547",
                        help="Junction ID to use as the intersection")
    parser.add_argument("--vehicles", default="4,8,16,32",
                        help="Comma-separated list of vehicle counts")
    parser.add_argument("--sim-dir", default="simulations",
                        help="Base simulation directory")
    return parser.parse_args()


class IntersectionInfo:
    """Holds detected intersection edge information."""
    def __init__(self):
        self.junction_id = ""
        self.approach_edges = []   # Ordered: W, S, E, N
        self.exit_edges = []       # Ordered: matching approach (W→E exit, S→N exit, etc.)
        self.approach_speeds = []  # Speed limit in m/s for each approach
        self.approach_lengths = [] # Length in meters for each approach
        self.predecessor_edges = [] # Edge BEFORE approach (for longer routes)
        self.successor_edges = []  # Edge AFTER exit (so vehicle stays alive for detection)


def detect_intersection(network_path, junction_id):
    """
    Parse a SUMO network XML to detect edges at a given junction.
    Returns an IntersectionInfo with approach/exit edges for straight-through routes.
    """
    tree = ET.parse(network_path)
    root = tree.getroot()
    
    info = IntersectionInfo()
    info.junction_id = junction_id
    
    # Build edge database (skip internal edges)
    edges_db = {}
    for edge in root.findall("edge"):
        eid = edge.get("id", "")
        func = edge.get("function", "")
        if func == "internal":
            continue
        efrom = edge.get("from", "")
        eto = edge.get("to", "")
        lanes = edge.findall("lane")
        if not lanes:
            continue
        lane0 = lanes[0]
        speed = float(lane0.get("speed", "13.89"))
        length = float(lane0.get("length", "100"))
        allow = lane0.get("allow", "all")
        # Check passenger access
        passenger_ok = True
        if allow != "all" and "passenger" not in allow:
            passenger_ok = False
        edges_db[eid] = {
            "from": efrom, "to": eto,
            "speed": speed, "length": length,
            "passenger_ok": passenger_ok,
            "num_lanes": len(lanes)
        }
    
    # Find approach (incoming) and exit (outgoing) vehicular edges
    approach_edges = []
    exit_edges = []
    for eid, einfo in edges_db.items():
        if not einfo["passenger_ok"]:
            continue
        if einfo["to"] == junction_id:
            approach_edges.append((eid, einfo))
        elif einfo["from"] == junction_id:
            exit_edges.append((eid, einfo))
    
    if len(approach_edges) < 2:
        print(f"ERROR: Junction {junction_id} has only {len(approach_edges)} vehicular approach edges")
        sys.exit(1)
    
    # Find straight-through connections to pair approach → exit
    connections = []
    for conn in root.findall("connection"):
        cfrom = conn.get("from", "")
        cto = conn.get("to", "")
        cdir = conn.get("dir", "")
        if cdir == "s":  # straight
            connections.append((cfrom, cto))
    
    # Build approach→exit mapping via straight connections
    approach_exit_pairs = []
    for app_eid, app_info in approach_edges:
        for cfrom, cto in connections:
            if cfrom == app_eid:
                # Find the exit edge
                for ex_eid, ex_info in exit_edges:
                    if ex_eid == cto:
                        approach_exit_pairs.append({
                            "approach": app_eid,
                            "exit": ex_eid,
                            "speed": app_info["speed"],
                            "length": app_info["length"],
                            "approach_from": app_info["from"]
                        })
                        break
    
    if len(approach_exit_pairs) < 2:
        print(f"WARNING: Only {len(approach_exit_pairs)} straight-through routes found.")
        print("Falling back to: pairing approach edges with any available exit edges.")
        # Fallback: pair approach with any exit
        used_exits = set()
        for app_eid, app_info in approach_edges:
            for ex_eid, ex_info in exit_edges:
                if ex_eid not in used_exits:
                    approach_exit_pairs.append({
                        "approach": app_eid,
                        "exit": ex_eid, 
                        "speed": app_info["speed"],
                        "length": app_info["length"],
                        "approach_from": app_info["from"]
                    })
                    used_exits.add(ex_eid)
                    break
    
    # Take up to 4 directions
    approach_exit_pairs = approach_exit_pairs[:4]
    
    # Pad to exactly 4 if needed (duplicate last pair)
    while len(approach_exit_pairs) < 4:
        approach_exit_pairs.append(approach_exit_pairs[-1])
    
    # Find predecessor edges for longer approach routes
    for pair in approach_exit_pairs:
        pred_junction = pair["approach_from"]
        # Find edges that lead TO pred_junction (vehicular only)
        for eid, einfo in edges_db.items():
            if not einfo["passenger_ok"]:
                continue
            if einfo["to"] == pred_junction and einfo["from"] != junction_id:
                pair["predecessor"] = eid
                pair["total_length"] = einfo["length"] + pair["length"]
                break
        else:
            pair["predecessor"] = None
            pair["total_length"] = pair["length"]
    
    # Find successor edges after exit (so vehicles stay alive for detection)
    for pair in approach_exit_pairs:
        exit_eid = pair["exit"]
        exit_to = edges_db[exit_eid]["to"] if exit_eid in edges_db else None
        pair["successor"] = None
        if exit_to:
            for eid, einfo in edges_db.items():
                if not einfo["passenger_ok"]:
                    continue
                if einfo["from"] == exit_to and eid != exit_eid:
                    pair["successor"] = eid
                    break
    
    # Populate IntersectionInfo
    for pair in approach_exit_pairs:
        info.approach_edges.append(pair["approach"])
        info.exit_edges.append(pair["exit"])
        info.approach_speeds.append(pair["speed"])
        info.approach_lengths.append(pair["length"])
        info.predecessor_edges.append(pair.get("predecessor"))
        info.successor_edges.append(pair.get("successor"))
    
    return info


def generate_route_file(info, num_vehicles, output_path):
    """Generate a SUMO route file for the given number of vehicles."""
    vehicles_per_dir = num_vehicles // 4
    if vehicles_per_dir < 1:
        vehicles_per_dir = 1
    
    # Use the real speed from the map for the vehicle type
    # But cap maxSpeed to a reasonable value for the benchmark
    avg_speed = sum(info.approach_speeds) / len(info.approach_speeds)
    max_speed_kmh = avg_speed * 3.6
    
    route_names = ["rW", "rS", "rE", "rN"]
    
    lines = []
    lines.append('<?xml version="1.0" encoding="UTF-8"?>')
    lines.append('<routes>')
    # Vehicle type uses realistic speed from real map
    lines.append(f'    <vType id="car" accel="2.6" decel="4.5" sigma="0.5" '
                 f'length="4.5" minGap="2.5" maxSpeed="{avg_speed:.2f}" color="1,1,0"/>')
    lines.append('')
    
    # Define routes (predecessor → approach → exit → successor)
    lines.append('    <!-- Routes through intersection -->')
    for i in range(4):
        approach = info.approach_edges[i]
        exit_e = info.exit_edges[i]
        pred = info.predecessor_edges[i]
        succ = info.successor_edges[i] if i < len(info.successor_edges) else None
        
        edge_list = []
        if pred and info.approach_lengths[i] < 80:
            edge_list.append(pred)
        edge_list.append(approach)
        edge_list.append(exit_e)
        if succ:
            edge_list.append(succ)  # Keep vehicle alive past exit edge
        
        edges = " ".join(edge_list)
        lines.append(f'    <route id="{route_names[i]}" edges="{edges}"/>')
    lines.append('')
    
    # Generate vehicles with staggered departures
    lines.append(f'    <!-- {num_vehicles} vehicles total, {vehicles_per_dir} per direction -->')
    
    # Interleave departures across directions for fairness
    depart_time = 0.0
    depart_interval = 2.0  # seconds between vehicles in same direction
    
    vehicles = []
    for pos in range(vehicles_per_dir):
        for dir_idx in range(4):
            veh_id = dir_idx * vehicles_per_dir + pos
            if veh_id >= num_vehicles:
                continue
            route = route_names[dir_idx]
            t = pos * depart_interval + dir_idx * 0.3  # Slight offset between directions
            vehicles.append((t, veh_id, route))
    
    # Sort by departure time
    vehicles.sort(key=lambda x: x[0])
    
    for t, veh_id, route in vehicles:
        lines.append(f'    <vehicle id="veh{veh_id}" type="car" route="{route}" '
                     f'depart="{t:.2f}" departPos="0" departSpeed="max"/>')
    
    lines.append('</routes>')
    
    with open(output_path, 'w') as f:
        f.write('\n'.join(lines) + '\n')
    
    print(f"  Generated {output_path} ({num_vehicles} vehicles)")


def generate_sumocfg(output_path, net_file="osm.net.xml"):
    """Generate SUMO configuration file."""
    content = f'''<?xml version="1.0" encoding="UTF-8"?>
<configuration>
    <input>
        <net-file value="{net_file}"/>
        <route-files value="intersection.rou.xml"/>
    </input>
    <time>
        <begin value="0"/>
        <end value="300"/>
        <step-length value="0.1"/>
    </time>
    <gui_only>
        <start value="true"/>
    </gui_only>
</configuration>
'''
    with open(output_path, 'w') as f:
        f.write(content)


def generate_launchd(output_path, net_file="osm.net.xml"):
    """Generate Veins/TraCI launchd configuration."""
    content = f'''<?xml version="1.0"?>
<launch>
    <copy file="intersection.sumocfg" type="config"/>
    <copy file="{net_file}"/>
    <copy file="intersection.rou.xml"/>
</launch>
'''
    with open(output_path, 'w') as f:
        f.write(content)


def generate_config_xml(output_path):
    """Generate radio propagation config for WAVE/802.11p."""
    content = '''<?xml version="1.0" encoding="UTF-8"?>
<root>
    <!-- Path Loss: Log-Distance model for urban intersection (NLOS) -->
    <!-- Alpha = 2.8 per Winner+ B1 urban micro-cell model -->
    <AnalogueModel type="SimplePathlossModel">
        <parameter name="alpha" type="double" value="2.8"/>
        <parameter name="carrierFrequency" type="double" value="5.890e9"/>
    </AnalogueModel>
    
    <!-- Decider: SNIR-based packet delivery for 802.11p -->
    <!-- Sensitivity: -85 dBm for 10 MHz @ 6 Mbps (QPSK 1/2) -->
    <Decider type="Decider80211p">
        <parameter name="centerFrequency" type="double" value="5.890e9"/>
        <parameter name="bandwidth" type="double" value="10e6"/>
        <parameter name="sensitivity" type="double" value="-85.0"/>
    </Decider>
    
    <Antenna type="AntennaPosition">
        <parameter name="offsetX" type="double" value="0.0"/>
        <parameter name="offsetY" type="double" value="0.0"/>
        <parameter name="offsetZ" type="double" value="1.895"/>
    </Antenna>
</root>
'''
    with open(output_path, 'w') as f:
        f.write(content)


def generate_ned_inet(output_path, pkg_name):
    """Generate NED file for UDP/INET simulation."""
    content = f'''package {pkg_name};

import inet.physicallayer.ieee80211.packetlevel.Ieee80211ScalarRadioMedium;
import benchmark.veins_inet.VeinsInetManager;
import inet.networklayer.configurator.ipv4.Ipv4NetworkConfigurator;

network IntersectionScenarioInet
{{
    parameters:
        @display("bgb=800,600");
    submodules:
        radioMedium: Ieee80211ScalarRadioMedium {{
            @display("p=50,50");
        }}
        manager: VeinsInetManager {{
            @display("p=50,100");
        }}
        configurator: Ipv4NetworkConfigurator {{
            @display("p=50,150");
            config = xml("<config><interface hosts=\\"**\\" address=\\"10.0.0.x\\" netmask=\\"255.255.255.0\\"/></config>");
        }}
}}
'''
    with open(output_path, 'w') as f:
        f.write(content)


def generate_ned_wave(output_path, pkg_name):
    """Generate NED file for WAVE/802.11p simulation."""
    content = f'''package {pkg_name};

import org.car2x.veins.nodes.RSU;
import org.car2x.veins.nodes.Scenario;

network IntersectionScenarioWave extends Scenario
{{
    parameters:
        @display("bgb=800,600");
}}
'''
    with open(output_path, 'w') as f:
        f.write(content)


def generate_omnetpp_udp_ini(output_path, info, num_vehicles):
    """Generate OMNeT++ INI file for UDP/INET simulation."""
    approach_str = ",".join(info.approach_edges)
    exit_str = ",".join(info.exit_edges)
    
    content = f'''[General]
network = IntersectionScenarioInet
sim-time-limit = 300s
debug-on-errors = true
cmdenv-express-mode = true

# Veins-INET manager
*.manager.updateInterval = 0.1s
*.manager.host = "localhost"
*.manager.port = 9999
*.manager.autoShutdown = true
*.manager.launchConfig = xmldoc("intersection.launchd.xml")
*.manager.moduleType = "benchmark.veins_inet.VeinsInetCar"
*.manager.moduleName = "node"
*.manager.moduleDisplayString = ""

# Node mobility
*.node[*].mobility.typename = "VeinsInetMobility"

# RAFT Application
*.node[*].numApps = 1
*.node[*].app[0].typename = "WillemtRaftApplication"
*.node[*].app[0].middleware.typename = "benchmark.raft.WillemtRaftApplication"
*.node[*].app[0].totalVehicles = {num_vehicles}
*.node[*].app[0].approachEdges = "{approach_str}"
*.node[*].app[0].exitEdges = "{exit_str}"
*.node[*].app[0].resultsFile = "raft_results.json"

# RAFT Timing
*.node[*].app[0].electionTimeoutBaseMs = 500
*.node[*].app[0].electionTimeoutJitterMs = 1000
*.node[*].app[0].requestTimeoutMs = 200
*.node[*].app[0].maxFailedElections = 100
*.node[*].app[0].fallbackWaitMinMs = 100
*.node[*].app[0].fallbackWaitMaxMs = 300
*.node[*].app[0].passConfirmationMs = 300
*.node[*].app[0].statusCollectionTimeoutMs = 200

# Wi-Fi
*.node[*].wlan[0].typename = "Ieee80211Interface"
*.node[*].wlan[0].radio.typename = "Ieee80211ScalarRadio"
*.node[*].wlan[0].radio.transmitter.communicationRange = 500m
*.node[*].wlan[0].radio.transmitter.power = 20mW
*.node[*].wlan[0].mac.dcf.channelAccess.cwMin = 7
*.node[*].wlan[0].radio.receiver.sensitivity = -85dBm
*.node[*].wlan[0].radio.centerFrequency = 5.9GHz
*.node[*].wlan[0].radio.bandwidth = 10MHz
'''
    with open(output_path, 'w') as f:
        f.write(content)


def generate_omnetpp_wave_ini(output_path, info, num_vehicles):
    """Generate OMNeT++ INI file for WAVE/802.11p simulation."""
    approach_str = ",".join(info.approach_edges)
    exit_str = ",".join(info.exit_edges)
    
    content = f'''[General]
network = IntersectionScenarioWave
sim-time-limit = 300s
debug-on-errors = false
cmdenv-express-mode = true
cmdenv-autoflush = true
print-undisposed = false
**.scalar-recording = false
**.vector-recording = false

# Veins manager
*.manager.updateInterval = 0.1s
*.manager.host = "localhost"
*.manager.port = 9999
*.manager.autoShutdown = true
*.manager.launchConfig = xmldoc("intersection.launchd.xml")
*.manager.moduleType = "org.car2x.veins.nodes.Car"
*.manager.moduleName = "node"
*.manager.moduleDisplayString = ""

# Mobility
*.node[*].veinsmobility.x = 0
*.node[*].veinsmobility.y = 0
*.node[*].veinsmobility.z = 0
*.node[*].veinsmobility.setHostSpeed = false
*.node[*].veinsmobility0.x = 0
*.node[*].veinsmobility0.y = 0
*.node[*].veinsmobility0.z = 0
*.node[*].veinsmobility0.setHostSpeed = false

# World / Playground
*.playgroundSizeX = 2500m
*.playgroundSizeY = 2500m
*.playgroundSizeZ = 50m

# Connection Manager
*.connectionManager.sendDirect = true
*.connectionManager.maxInterfDist = 2600m
*.connectionManager.drawMaxIntfDist = false

# MAC 1609.4
*.**.nic.mac1609_4.useServiceChannel = false
*.**.nic.mac1609_4.txPower = 20mW
*.**.nic.mac1609_4.bitrate = 6Mbps

# RAFT Application
*.node[*].applType = "benchmark.raft.WillemtRaftWaveApplication"
*.node[*].appl.totalVehicles = {num_vehicles}
*.node[*].appl.approachEdges = "{approach_str}"
*.node[*].appl.exitEdges = "{exit_str}"
*.node[*].appl.resultsFile = "raft_results.json"

# RAFT Timing
*.node[*].appl.electionTimeoutBaseMs = 500
*.node[*].appl.electionTimeoutJitterMs = 1000
*.node[*].appl.requestTimeoutMs = 200
*.node[*].appl.maxFailedElections = 100
*.node[*].appl.fallbackWaitMinMs = 100
*.node[*].appl.fallbackWaitMaxMs = 300
*.node[*].appl.passConfirmationMs = 300
*.node[*].appl.statusCollectionTimeoutMs = 200

# 802.11p NIC
*.node[*].nic.phy80211p.analogueModels = xmldoc("config.xml", "//AnalogueModel")
*.node[*].nic.phy80211p.decider = xmldoc("config.xml", "//Decider")
*.node[*].nic.phy80211p.sensitivity = -85dBm
*.node[*].nic.phy80211p.maxTXPower = 200mW
*.node[*].nic.phy80211p.useThermalNoise = true
*.node[*].nic.phy80211p.thermalNoise = -110dBm
*.node[*].nic.phy80211p.useNoiseFloor = true
*.node[*].nic.phy80211p.noiseFloor = -98dBm
*.node[*].nic.phy80211p.usePropagationDelay = true
*.node[*].nic.phy80211p.minPowerLevel = -110dBm
'''
    with open(output_path, 'w') as f:
        f.write(content)


def patch_network_for_passenger(network_path, output_path):
    """
    Patch the OSM network to allow passenger vehicles on all lanes.
    Some OSM-imported networks restrict certain edges to pedestrian/bicycle only.
    """
    import shutil
    
    tree = ET.parse(network_path)
    root = tree.getroot()
    
    patched = 0
    for edge in root.findall("edge"):
        func = edge.get("function", "")
        if func == "internal":
            continue
        for lane in edge.findall("lane"):
            allow = lane.get("allow", "")
            if allow and "passenger" not in allow and allow != "all":
                # Add passenger to allowed vehicles
                new_allow = allow + " passenger"
                lane.set("allow", new_allow)
                patched += 1
    
    tree.write(output_path, xml_declaration=True, encoding="UTF-8")
    print(f"  Patched network: {patched} lanes updated for passenger access")


def setup_scenario(info, num_vehicles, sim_dir, network_path):
    """Set up a complete intersection scenario directory."""
    dir_name = os.path.join(sim_dir, f"intersection_{num_vehicles}")
    os.makedirs(dir_name, exist_ok=True)
    
    pkg_name = f"intersection_{num_vehicles}"
    net_basename = "osm.net.xml"
    net_dest = os.path.join(dir_name, net_basename)
    
    print(f"\nSetting up {dir_name}/  ({num_vehicles} vehicles)")
    
    # Copy and patch network file
    patch_network_for_passenger(network_path, net_dest)
    
    # Generate all config files
    generate_route_file(info, num_vehicles, os.path.join(dir_name, "intersection.rou.xml"))
    generate_sumocfg(os.path.join(dir_name, "intersection.sumocfg"), net_basename)
    generate_launchd(os.path.join(dir_name, "intersection.launchd.xml"), net_basename)
    generate_config_xml(os.path.join(dir_name, "config.xml"))
    generate_ned_inet(os.path.join(dir_name, "IntersectionScenarioInet.ned"), pkg_name)
    generate_ned_wave(os.path.join(dir_name, "IntersectionScenarioWave.ned"), pkg_name)
    generate_omnetpp_udp_ini(os.path.join(dir_name, "omnetpp_udp.ini"), info, num_vehicles)
    generate_omnetpp_wave_ini(os.path.join(dir_name, "omnetpp_wave.ini"), info, num_vehicles)
    
    print(f"  ✓ Complete: {dir_name}/")


def main():
    args = parse_args()
    
    vehicle_counts = [int(v.strip()) for v in args.vehicles.split(",")]
    
    print("=" * 60)
    print("RAFT V2V Intersection Benchmark — Scenario Setup")
    print("=" * 60)
    print(f"Network:  {args.network}")
    print(f"Junction: {args.junction}")
    print(f"Vehicles: {vehicle_counts}")
    print()
    
    # Detect intersection edges from network file
    print("Detecting intersection edges...")
    info = detect_intersection(args.network, args.junction)
    
    print(f"\nIntersection: {info.junction_id}")
    dirs = ["W", "S", "E", "N"]
    for i in range(len(info.approach_edges)):
        d = dirs[i] if i < len(dirs) else f"Dir{i}"
        pred = info.predecessor_edges[i] or "(none)"
        print(f"  {d}: approach={info.approach_edges[i]} "
              f"exit={info.exit_edges[i]} "
              f"speed={info.approach_speeds[i]:.1f}m/s "
              f"length={info.approach_lengths[i]:.1f}m "
              f"pred={pred}")
    
    # Generate scenarios for each vehicle count
    for num_vehicles in vehicle_counts:
        setup_scenario(info, num_vehicles, args.sim_dir, args.network)
    
    print("\n" + "=" * 60)
    print("All scenarios generated successfully!")
    print(f"Edge config for INI files:")
    print(f'  approachEdges = "{",".join(info.approach_edges)}"')
    print(f'  exitEdges     = "{",".join(info.exit_edges)}"')
    print("=" * 60)


if __name__ == "__main__":
    main()
