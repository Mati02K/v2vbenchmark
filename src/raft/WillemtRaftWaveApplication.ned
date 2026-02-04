package benchmark.raft;

import org.car2x.veins.modules.application.ieee80211p.DemoBaseApplLayer;

// RAFT intersection coordination over WAVE/802.11p transport
simple WillemtRaftWaveApplication extends DemoBaseApplLayer
{
    parameters:
        @class(WillemtRaftWaveApplication);
        
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
        
        // Output file name
        string resultsFile = default("raft_results.json");
}
