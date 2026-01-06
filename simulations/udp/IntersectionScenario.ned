package benchmark.simulations.udp;

//#if INET_VERSION < 0x0403
import inet.physicallayer*.ieee80211.packetlevel.Ieee80211DimensionalRadioMedium;
//#else
import inet.physicallayer*.wireless.ieee80211.packetlevel.Ieee80211DimensionalRadioMedium;
//#endif

import benchmark.veins_inet.VeinsInetCar;
import benchmark.veins_inet.VeinsInetManager;

//#if INET_VERSION < 0x0403
import inet.visualizer*.integrated.IntegratedVisualizer;
//#else
import inet.visualizer*.common.IntegratedVisualizer;
//#endif

import inet.environment.common.PhysicalEnvironment;
import org.car2x.veins.visualizer.roads.RoadsCanvasVisualizer;
import org.car2x.veins.visualizer.roads.RoadsOsgVisualizer;

network IntersectionScenario
{
    parameters:
        bool useOsg = default(false);
        @display("bgb=319,384");
    submodules:
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
