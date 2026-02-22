#!/usr/bin/env python3
"""
Generate route files and SUMO configuration for WAVE RAFT benchmark.
"""
import sys
import os

def generate_routes(num_vehicles, output_file):
    """Generate route file with specified number of vehicles."""
    vehicles_per_direction = num_vehicles // 4
    
    with open(output_file, 'w') as f:
        f.write('<?xml version="1.0" encoding="UTF-8"?>\n')
        f.write('<routes>\n')
        f.write('    <vType id="car" accel="2.6" decel="4.5" sigma="0.5" length="4.5" minGap="2.5" maxSpeed="14" color="1,1,0"/>\n\n')
        
        # Routes from each direction
        f.write('    <!-- Routes from each direction -->\n')
        f.write('    <route id="rN" edges="N2C C2S"/>\n')
        f.write('    <route id="rS" edges="S2C C2N"/>\n')
        f.write('    <route id="rE" edges="E2C C2W"/>\n')
        f.write('    <route id="rW" edges="W2C C2E"/>\n\n')
        
        f.write(f'    <!-- {num_vehicles} vehicles total -->\n')
        
        routes = ['rN', 'rS', 'rE', 'rW']
        vehicle_id = 0
        
        for dir_idx, route in enumerate(routes):
            for pos_idx in range(vehicles_per_direction):
                depart_pos = 280 - (pos_idx * 10)  # Stagger positions
                f.write(f'    <vehicle id="veh{vehicle_id}" type="car" route="{route}" depart="0" departSpeed="max" departPos="{depart_pos}"/>\n')
                vehicle_id += 1
        
        f.write('</routes>\n')
    
    print(f"Generated {output_file} with {num_vehicles} vehicles")

def generate_launchd(sumo_cfg, route_file, output_file):
    """Generate SUMO launchd.xml for Veins."""
    with open(output_file, 'w') as f:
        f.write('<?xml version="1.0"?>\n')
        f.write('<launch>\n')
        f.write(f'    <copy file="{sumo_cfg}" type="config"/>\n')
        f.write('    <copy file="intersection.net.xml"/>\n')
        f.write(f'    <copy file="{route_file}"/>\n')
        f.write('</launch>\n')

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: generate_config.py <num_vehicles>")
        sys.exit(1)
    
    num_vehicles = int(sys.argv[1])
    output_dir = "."  # Current directory (script is run from simulations/raftwave)
    
    # Generate routes file with specific naming convention
    routes_file = f"intersection_{num_vehicles}veh.rou.xml"
    generate_routes(num_vehicles, routes_file)
    
    # Generate launchd.xml
    launchd_file = "intersection.launchd.xml"
    generate_launchd("intersection.sumo.cfg", launchd_file)
