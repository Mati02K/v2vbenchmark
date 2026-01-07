package benchmark.simulations.tcp;

import inet.physicallayer.ieee80211.packetlevel.Ieee80211DimensionalRadioMedium;

import benchmark.veins_inet.VeinsInetCar;
import benchmark.veins_inet.VeinsInetManager;

import inet.visualizer.integrated.IntegratedVisualizer;


import inet.environment.common.PhysicalEnvironment;
import org.car2x.veins.visualizer.roads.RoadsCanvasVisualizer;
import org.car2x.veins.visualizer.roads.RoadsOsgVisualizer;
import inet.networklayer.configurator.ipv4.Ipv4NetworkConfigurator;

network IntersectionScenario
{
    parameters:
        bool useOsg = default(false);
        @display("bgb=319,384");
        
    submodules:
        configurator: Ipv4NetworkConfigurator {
            @display("p=64,128");
        }
        radioMedium: Ieee80211DimensionalRadioMedium {
            @display("p=64,224");
        }
        manager: VeinsInetManager {
            @display("p=192,320");
        }
        visualizer: IntegratedVisualizer {
            @display("p=64,320");
        }
        physicalEnvironment: PhysicalEnvironment {
            @display("p=192,224");
        }
        roadsCanvasVisualizer: RoadsCanvasVisualizer {
            @display("p=64,416");
        }
        roadsOsgVisualizer: RoadsOsgVisualizer if useOsg {
            @display("p=192,416");
        }
        node[0]: VeinsInetCar;
}