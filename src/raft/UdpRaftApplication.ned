package benchmark.raft;

import benchmark.veins_inet.VeinsInetApplicationBase;

simple UdpRaftApplication extends VeinsInetApplicationBase
{
    parameters:
        @class(UdpRaftApplication);
        
        // Vehicle configuration
        int totalVehicles = default(4);
        
        // RAFT timing (milliseconds)
        int electionTimeoutBaseMs = default(500);
        int electionTimeoutJitterMs = default(1000);
        int requestTimeoutMs = default(200);
        int maxFailedElections = default(100);
        
        // Fallback timing (milliseconds) 
        int fallbackWaitMinMs = default(100);
        int fallbackWaitMaxMs = default(300);
        
        // Pass confirmation time (milliseconds)
        int passConfirmationMs = default(300);
        
        // Status collection timeout (milliseconds)
        int statusCollectionTimeoutMs = default(200);

        // Cluster formation fallback timeout (milliseconds) — if RAFT cluster not formed by this time after stopping, activate fallback
        int fallbackClusterTimeoutMs = default(30000);
        
        // Distance-based intersection stopping
        double intersectionStopDistance @unit(m) = default(25m);  // Stop within this distance of junction
        int arrivalWaitTimeMs = default(3000);  // Wait time for vehicles to gather before scheduling
        int discoveryWaitMs = default(3000);    // Time first stopped vehicle waits before forming cluster (increase for far-start scenarios)
        int clusterFormationDelayMs = default(0); // Extra delay after first stop before forming (lets others arrive)
        int mergeCooldownMs = default(2000);     // Min ms between merges (prevents merge storms)
        int vehicleLeftTimeoutMs = default(1500); // If no VEHICLE_LEFT within this ms, assume vehicle left
        int invitationIntervalStoppedMs = default(10);   // When stopped: invitation interval ms (10ms + jitter); configurable
        
        // Dynamic cluster formation (matching WAVE)
        double clusterTriggerDistance = default(100.0);  // meters from junction center
        double discoveryBeaconInterval = default(0.3);   // seconds between beacons
        
        // Intersection edge configuration (comma-separated edge IDs)
        // Approach edges: edges leading INTO the intersection (one per direction W,S,E,N)
        string approachEdges = default("W2C,S2C,E2C,N2C");
        // Exit edges: edges leading OUT of the intersection (one per direction, matching approach order)
        string exitEdges = default("C2E,C2N,C2W,C2S");
        
        // Output file name
        string resultsFile = default("raft_results.json");
        // When to force-close results file (sim time in seconds). Ensures valid JSON even if not all
        // vehicles stop before sim-time-limit. Use 0 to rely on vehicle count only. Default 299 = 1s before 300s limit.
        double resultsFileCloseAtSec = default(299);
}
