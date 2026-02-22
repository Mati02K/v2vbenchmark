package benchmark.raft;

import benchmark.veins_inet.VeinsInetApplicationBase;

simple WillemtRaftApplication extends VeinsInetApplicationBase
{
    parameters:
        @class(WillemtRaftApplication);
        
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
        
        // Intersection edge configuration (comma-separated edge IDs)
        // Approach edges: edges leading INTO the intersection (one per direction W,S,E,N)
        string approachEdges = default("W2C,S2C,E2C,N2C");
        // Exit edges: edges leading OUT of the intersection (one per direction, matching approach order)
        string exitEdges = default("C2E,C2N,C2W,C2S");
        
        // Output file name
        string resultsFile = default("raft_results.json");
}
