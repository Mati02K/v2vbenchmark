#pragma once

#include "veins_inet/VeinsInetApplicationBase.h"
#include "veins_inet/VeinsInetMobility.h"
#include "veins/modules/mobility/traci/TraCICommandInterface.h"
#include <fstream>
#include <map>
#include <set>

extern "C" {
#include "../../third_party/raft/raft_types.h"
#include "../../third_party/raft/raft.h"
}

/**
 * RAFT-based intersection coordination using willemt/raft library
 * UDP over WiFi transport version
 * 
 * DYNAMIC CLUSTER FORMATION PROTOCOL (identical to WAVE):
 * Phase 1 (DISCOVERY): Broadcast lightweight beacons to discover peers
 * Phase 2 (FORMATION): First vehicle at intersection triggers cluster formation
 * Phase 3 (COORDINATION): RAFT leader collects VehicleProposals, schedules fair pass order
 */
class WillemtRaftApplication : public veins::VeinsInetApplicationBase
{
public:
    WillemtRaftApplication();
    virtual ~WillemtRaftApplication();

protected:
    virtual bool startApplication() override;
    virtual bool stopApplication() override;
    virtual void processPacket(std::shared_ptr<inet::Packet> pk) override;

private:
    // ============ CLUSTER PHASE (matching WAVE) ============
    enum ClusterPhase {
        PHASE_DISCOVERY,     // Broadcasting/receiving beacons
        PHASE_FORMATION,     // Cluster forming, RAFT initializing
        PHASE_COORDINATION,  // RAFT running, scheduling passes
        PHASE_PASSED         // Vehicle has exited intersection
    };
    ClusterPhase clusterPhase_;
    
    // ============ RAFT CORE ============
    raft_server_t* raftServer_;
    
    // Vehicle info
    int myId_;
    int myRaftNodeId_;
    std::string myRoute_;
    std::string myLane_;
    
    // Mobility
    veins::VeinsInetMobility* mobility_;
    veins::TraCICommandInterface* traci_;
    veins::TraCICommandInterface::Vehicle* traciVehicle_;
    
    // ============ DISCOVERY STATE (matching WAVE) ============
    std::set<int> discoveredPeers_;     // Vehicle IDs discovered via beacons
    double discoveryBeaconInterval_;
    double clusterTriggerDistance_;
    bool clusterFormed_;
    
    void sendDiscoveryBeacon();
    void handleDiscoveryBeacon(int senderId, uint8_t senderPhase);
    void checkClusterTrigger();
    
    void broadcastClusterForm();
    void handleClusterForm(const std::vector<uint8_t>& data, int senderId);
    void handleClusterExists(const std::vector<uint8_t>& data, int senderId);
    void mergeIntoCluster(const std::set<int>& members);
    void broadcastClusterExists();
    void formCluster(const std::set<int>& members);
    
    // ============ INTERSECTION STATE ============
    bool isLeader_;
    bool hasStoppedAtIntersection_;
    bool hasPassedIntersection_;
    std::string intersectionEdge_;
    
    bool wayOfSight_;
    std::set<int> activeVehicles_;
    std::map<int, bool> collectedWayOfSight_;
    bool waitingForStatus_;
    int statusResponseCount_;
    int vehicleInFrontOfMe_;
    
    // Dynamic intersection edges (loaded from NED parameters)
    std::set<std::string> intersectionEdges_;  // All approach + exit edges
    std::vector<std::string> approachEdgeList_;  // Ordered: dir0, dir1, dir2, dir3
    std::set<std::string> exitEdges_;  // Exit edges only
    std::vector<std::string> exitEdgeList_;  // Ordered: matching approach order
    void parseEdgeParameters();
    
    // ============ RAFT LOG ENTRY TYPES ============
    enum LogEntryType : uint8_t {
        PASS_COMMAND = 1,      // OLD - will be removed
        STATUS_REPORT = 2,     // NEW - vehicle status consensus
        PASS_ORDER = 3,        // NEW - complete pass order (batch-based)
        VEHICLE_LEFT = 4       // NEW - vehicle departure via RAFT consensus
    };

    struct PassCommandEntry {
        int vehicleId;
        simtime_t proposedTime;
    };
    
    struct VehicleStatus {
        int vehicleId;
        bool wayOfSight;
        char lane[64];
        int positionInLane;
        int direction; // 0=Straight, 1=Left, 2=Right
    };
    
    struct StatusReportEntry {
        int numVehicles;
        VehicleStatus statuses[32];  // Max 32 vehicles
    };
    
    // VehicleProposal: sent by each vehicle to leader for scheduling
    struct VehicleProposal {
        int vehicleId;
        char laneEdgeId[64];     // Approach edge ID
        double positionOnLane;   // Distance from edge start (meters)
        double speed;            // Current speed (m/s)
        int laneIndex;           // Direction: 0=W, 1=S, 2=E, 3=N
        int intendedTurn;        // 0=STRAIGHT, 1=LEFT, 2=RIGHT
        bool isFirstInLane;      // Frontmost vehicle on this lane?
        int blockedByVehicleId;  // ID of vehicle blocking this one (-1 if none)
        double waitingTimeMs;    // How long this vehicle has been waiting
        double distanceToJunction; // Distance to junction center (meters)
    };
    
    // Batch-based pass scheduling
    struct PassBatch {
        int numVehicles;
        int vehicleIds[8];   // Vehicles that can pass simultaneously (max 8 per batch)
    };
    
    struct PassScheduleEntry {
        int numBatches;
        PassBatch batches[16]; // Up to 16 sequential batches (supports up to 64 vehicles)
    };
    
    struct VehicleLeftEntry {
        int vehicleId;
        int batchId;
    };
    
    // Legacy PassOrderEntry (flat order, kept for log compatibility)
    struct PassOrderEntry {
        int numVehicles;
        int order[32];  // Vehicle IDs in pass order
    };

    std::map<raft_index_t, uint8_t*> pendingProposals_;
    raft_index_t lastAppliedIndex_;
    
    // NEW: Store committed status and pass order
    std::map<int, VehicleStatus> committedStatuses_;
    std::map<int, VehicleProposal> collectedProposals_;  // Proposals from status collection
    PassOrderEntry committedPassOrder_;
    PassScheduleEntry committedSchedule_;   // Batch-based schedule
    int waitingForVehicle_;  // Vehicle ID I'm waiting for to leave
    bool hasCommittedOrder_;  // Whether pass order has been committed
    
    // Batch execution state
    int currentBatch_;                   // Which batch is currently passing
    int myBatch_;                        // The batch this vehicle is assigned to cross in
    std::set<int> vehiclesLeftInBatch_;  // Track which vehicles left in current batch
    int myLaneIndex_;                    // Direction: 0=W, 1=S, 2=E, 3=N

    // ============ FALLBACK STATE ============
    bool isFallbackMode_;
    int failedElectionCount_;
    raft_term_t lastCheckedTerm_;
    
    // ============ CONFIGURABLE PARAMETERS ============
    int totalVehicles_;
    int electionTimeoutBaseMs_;
    int electionTimeoutJitterMs_;
    int requestTimeoutMs_;
    double intersectionStopDistance_;    // Distance to junction center for stopping (meters)
    int arrivalWaitTimeMs_;              // Time to wait for vehicles to gather before scheduling
    int maxFailedElections_;
    int fallbackWaitMinMs_;
    int fallbackWaitMaxMs_;
    int passConfirmationMs_;
    int statusCollectionTimeoutMs_;
    std::string resultsFileName_;
    
    static constexpr double CHECK_INTERVAL = 0.05;
    static constexpr double RAFT_PERIODIC_INTERVAL = 0.02;
    
    // ============ METRICS TRACKING ============
    simtime_t timeArrived_;
    simtime_t timeStopped_;
    simtime_t timeClusterFormed_;   // When RAFT cluster was formed
    simtime_t timeElected_;
    simtime_t timeOrderCommitted_;  // When PASS_ORDER is committed to quorum
    simtime_t timeStartedMoving_;
    simtime_t timePassed_;
    
    // Arrival wait state
    bool waitingForVehiclesToArrive_;  // Flag for arrival wait state
    int messagesSent_;
    int messagesReceived_;
    int electionRounds_;
    bool wasElectedLeader_;
    std::string coordinationMethod_;
    int logEntriesProposed_;
    int logEntriesCommitted_;
    bool metricsWritten_;
    
    static std::ofstream resultsFile_;
    static bool resultsFileOpened_;
    static int vehiclesCompleted_;
    static int totalVehiclesStatic_;
    static std::string resultsFileNameStatic_;
    
    // ============ RAFT CALLBACKS ============
    static int sendRequestVote(raft_server_t* raft, void* user_data, 
                               raft_node_t* node, msg_requestvote_t* msg);
    static int sendAppendEntries(raft_server_t* raft, void* user_data,
                                 raft_node_t* node, msg_appendentries_t* msg);
    static int logOffer(raft_server_t* raft, void* user_data,
                        raft_entry_t* entry, raft_index_t entry_idx);
    static int applylog(raft_server_t* raft, void* user_data,
                        raft_entry_t* entry, raft_index_t entry_idx);
    static void log(raft_server_t* raft, raft_node_t* node,
                   void* user_data, const char* buf);
    static int persistVote(raft_server_t* raft, void* user_data, raft_node_id_t vote);
    
    int doSendRequestVote(raft_node_t* node, msg_requestvote_t* msg);
    int doSendAppendEntries(raft_node_t* node, msg_appendentries_t* msg);
    int doLogOffer(raft_entry_t* entry, raft_index_t entry_idx);
    int doApplyLog(raft_entry_t* entry, raft_index_t entry_idx);
    
    // ============ RAFT MESSAGE HANDLING ============
    void handleRequestVote(const std::vector<uint8_t>& data, const std::string& packetName);
    void handleRequestVoteResponse(const std::vector<uint8_t>& data, const std::string& packetName);
    void handleAppendEntries(const std::vector<uint8_t>& data, const std::string& packetName);
    void handleAppendEntriesResponse(const std::vector<uint8_t>& data, const std::string& packetName);
    
    int extractTargetFromPacketName(const std::string& packetName);
    int extractSenderFromPacketName(const std::string& packetName);
    
    std::vector<uint8_t> serializeRequestVote(msg_requestvote_t* msg);
    std::vector<uint8_t> serializeRequestVoteResponse(msg_requestvote_response_t* msg);
    std::vector<uint8_t> serializeAppendEntries(msg_appendentries_t* msg);
    std::vector<uint8_t> serializeAppendEntriesResponse(msg_appendentries_response_t* msg);
    
    void deserializeRequestVote(const std::vector<uint8_t>& data, msg_requestvote_t* msg);
    void deserializeRequestVoteResponse(const std::vector<uint8_t>& data, msg_requestvote_response_t* msg);
    void deserializeAppendEntries(const std::vector<uint8_t>& data, msg_appendentries_t* msg);
    void deserializeAppendEntriesResponse(const std::vector<uint8_t>& data, msg_appendentries_response_t* msg);
    
    // ============ COORDINATION PROTOCOL WITH QUORUM ============
    void sendStatusRequest();
    void handleStatusRequest(int fromLeader);
    void sendStatusResponse(int toLeader, bool wayOfSight);
    void handleStatusResponse(int fromVehicle, bool wayOfSight);
    void sendVehiclePassed();
    void handleVehiclePassed(int vehicleId);
    
    // NEW: Two-phase consensus functions
    void collectStatusAndDecide();
    void proposeStatusReport();
    void proposePassOrder();
    std::vector<int> createDeterministicOrder();
    void executePassOrder();
    void applyCommittedPassOrder();
    void checkIfPreviousVehicleLeft(int previousVehicleId);
    int findMyPositionInOrder();
    int getLaneIndex(const std::string& lane);
    
    // Batch-based scheduling helpers
    VehicleProposal buildMyProposal();
    int detectBlockingVehicle();
    double calculateDistanceToJunction();
    bool movementsConflict(int laneA, int turnA, int laneB, int turnB);
    void checkBatchAdvance();
    void applyVehicleLeftFromRaft(int vehicleId, int batchId);
    
    // OLD: Will be removed
    int selectVehicleToPass();
    void proposePassCommand(int vehicleId);
    void executePassCommand(int vehicleId);
    
    // NEW: Vehicle-left via RAFT consensus
    void sendVehicleLeft();
    void handleVehicleLeft(int vehicleId);
    void proposeVehicleLeft(int vehicleId, int batchId);
    
    void calculateWayOfSight();
    
    // ============ INTERSECTION COORDINATION ============
    void checkAndStopAtIntersection();
    bool isAtIntersection() const;
    bool isNearJunction() const;           // Distance-based junction detection
    double getDistanceToJunction() const;  // Get actual distance to junction
    bool hasPassedIntersectionEdge() const;
    void onBecameLeader();
    void onLostLeadership();
    void resumeMovement();
    void checkIfLeftIntersection();
    void stopVehicle();
    void handleFallback();
    void processRaftPeriodic();
    
    void outputMetricsJSON();
    static void openResultsFile(const std::string& filename);
    static void closeResultsFile();
    
    raft_node_id_t getNodeIdFromVehicleId(int vehicleId) const;
    int getVehicleIdFromNodeId(raft_node_id_t nodeId) const;
};
