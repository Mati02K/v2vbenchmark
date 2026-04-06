package benchmark.raft;

import org.car2x.veins.modules.application.ieee80211p.DemoBaseApplLayer;

// RAFT intersection coordination over WAVE/802.11p transport
simple WaveRaftApplication extends DemoBaseApplLayer
{
    parameters:
        @class(WaveRaftApplication);

        // Vehicle configuration (scenario-specific)
        int totalVehicles = default(4);

        // Scenario geometry
        double intersectionStopDistance @unit(m) = default(25m);
        double clusterTriggerDistance = default(100.0);  // meters from junction center
        double discoveryBeaconInterval = default(0.3);   // seconds between beacons

        // Intersection edge configuration (comma-separated edge IDs)
        string approachEdges = default("W2C,S2C,E2C,N2C");
        string exitEdges = default("C2E,C2N,C2W,C2S");

        // Output
        string resultsFile = default("raft_results.json");
        double resultsFileCloseAtSec = default(299);

        // Per-run override
        bool isPriorityVehicle = default(false);
}
