package benchmark.udp;

import org.car2x.veins.modules.mobility.traci.TraCIScenarioManager;

simple UdpTraCIManager extends TraCIScenarioManager
{
    parameters:
        @class(veins::TraCIScenarioManager);
}
