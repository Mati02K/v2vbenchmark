package benchmark.raft;

import benchmark.veins_inet.VeinsInetApplicationBase;

simple WillemtRaftApplication extends VeinsInetApplicationBase
{
    parameters:
        @class(WillemtRaftApplication);
        
        // Vehicle configuration
        int totalVehicles = default(4);
        
        // RAFT timing (milliseconds)
        int electionTimeoutBaseMs = default(150);
        int electionTimeoutJitterMs = default(50);
        int requestTimeoutMs = default(50);
        int maxFailedElections = default(6);
        
        // Fallback timing (milliseconds) 
        int fallbackWaitMinMs = default(100);
        int fallbackWaitMaxMs = default(300);
        
        // Pass confirmation time (milliseconds)
        int passConfirmationMs = default(300);
        
        // Status collection timeout (milliseconds)
        int statusCollectionTimeoutMs = default(200);
        
        // Output file name
        string resultsFile = default("raft_results.json");
}
