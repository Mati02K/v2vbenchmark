package benchmark.raft;

import benchmark.veins_inet.VeinsInetApplicationBase;

//
// GREEDY RAFT Application for intersection coordination
//
// KEY DIFFERENCE from WillemtRaftApplication:
// - Leader prioritizes vehicles in their OWN LANE first
// - If leader is at back of lane, commands front vehicles to pass first
// - Once entire lane is cleared, leader passes
// - Then new election happens for remaining vehicles
//
// This is "greedy" because the leader helps their lane mates before
// allowing vehicles from other lanes to pass.
//
simple GreedyRaftApplication extends VeinsInetApplicationBase
{
    parameters:
        @class(GreedyRaftApplication);
        
        // Vehicle configuration
        int totalVehicles = default(4);
        
        // RAFT timing (milliseconds)
        // Higher jitter and more failed elections allowed for greedy algorithm
        // since we have many re-elections as each leader passes
        int electionTimeoutBaseMs = default(150);
        int electionTimeoutJitterMs = default(100);  // More spread to reduce vote splitting
        int requestTimeoutMs = default(50);
        int maxFailedElections = default(20);  // Allow more retries before fallback
        
        // Fallback timing (milliseconds) 
        int fallbackWaitMinMs = default(100);
        int fallbackWaitMaxMs = default(300);
        
        // Pass confirmation time (milliseconds)
        int passConfirmationMs = default(300);
        
        // Status collection timeout (milliseconds)
        int statusCollectionTimeoutMs = default(200);
        
        // Output file name
        string resultsFile = default("greedy_raft_results.json");
}
