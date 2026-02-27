package simple_intersection_32;

import inet.physicallayer.ieee80211.packetlevel.Ieee80211ScalarRadioMedium;
import benchmark.veins_inet.VeinsInetManager;
import inet.networklayer.configurator.ipv4.Ipv4NetworkConfigurator;

network IntersectionScenarioInet
{
    parameters:
        @display("bgb=800,600");
    submodules:
        radioMedium: Ieee80211ScalarRadioMedium {
            @display("p=50,50");
        }
        manager: VeinsInetManager {
            @display("p=50,100");
        }
        configurator: Ipv4NetworkConfigurator {
            @display("p=50,150");
            config = xml("<config><interface hosts=\"**\" address=\"10.0.0.x\" netmask=\"255.255.255.0\"/></config>");
        }
}
