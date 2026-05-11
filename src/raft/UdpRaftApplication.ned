package benchmark.raft;

import benchmark.veins_inet.VeinsInetApplicationBase;

simple UdpRaftApplication extends VeinsInetApplicationBase
{
    parameters:
        @class(UdpRaftApplication);

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

        // Cluster formation mode: "cluster" or "allVehicles"
        string clusterMode = default("cluster");

        // Per-run override
        bool isPriorityVehicle = default(false);

        // Test knob: if true, start a new RAFT round even when all vehicles are already scheduled
        bool allowMultipleRounds = default(false);
}
