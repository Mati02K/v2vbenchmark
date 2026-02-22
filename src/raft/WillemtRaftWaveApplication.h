#pragma once

#include "veins/modules/application/ieee80211p/DemoBaseApplLayer.h"
#include "veins/modules/mobility/traci/TraCIMobility.h"
#include "veins/modules/mobility/traci/TraCICommandInterface.h"
#include <fstream>
#include <map>
#include <set>
#include <vector>

extern "C" {
#include "../../third_party/raft/raft_types.h"
#include "../../third_party/raft/raft.h"
}

using namespace veins;

/**
 * RAFT-based intersection coordination using willemt/raft library
 * WAVE/802.11p transport version
 * 
 * DYNAMIC CLUSTER FORMATION PROTOCOL:
 * Phase 1 (DISCOVERY): Broadcast lightweight beacons to discover peers
 * Phase 2 (FORMATION): First vehicle ≤ triggerDistance triggers cluster formation
 * Phase 3 (COORDINATION): RAFT leader collects VehicleProposals, schedules fair pass order
 */
class WillemtRaftWaveApplication : public DemoBaseApplLayer
{
public:
    WillemtRaftWaveApplication();
    virtual ~WillemtRaftWaveApplication();

protected:
    virtual void initialize(int stage) override;
    virtual void finish() override;
    virtual void onWSM(BaseFrame1609_4* wsm) override;
    virtual void handleSelfMsg(cMessage* msg) override;
    virtual void handlePositionUpdate(cObject* obj) override;

private:
    // ============ CLUSTER PHASE ============
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
    std::string myLane_;        // Approach edge this vehicle is on
    int myLaneIndex_;           // Direction index: 0=W, 1=S, 2=E, 3=N
    
    // Mobility
    TraCIMobility* mobility_;
    TraCICommandInterface* traci_;
    TraCICommandInterface::Vehicle* traciVehicle_;
    
    // ============ DISCOVERY STATE ============
    std::set<int> discoveredPeers_;     // Vehicle IDs discovered via beacons
    cMessage* discoveryTimer_;          // Periodic beacon timer
    double discoveryBeaconInterval_;
    double clusterTriggerDistance_;
    bool clusterFormed_;

    // Junction center position (computed from network)
    double junctionX_;
    double junctionY_;
    bool junctionPosKnown_;
    
    void sendDiscoveryBeacon();
    void handleDiscoveryBeacon(int senderId);
    void checkClusterTrigger();
    double getDistanceToJunction() const;
    
    void broadcastClusterForm();
    void handleClusterForm(const std::vector<uint8_t>& data, int senderId);
    void handleClusterExists(const std::vector<uint8_t>& data, int senderId);  // FIX #2
    void mergeIntoCluster(const std::set<int>& members);  // FIX #2
    void broadcastClusterExists();  // FIX #2: continuous cluster beaconing
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
        PASS_COMMAND = 1,
        STATUS_REPORT = 2,
        PASS_ORDER = 3,
        VEHICLE_LEFT = 4    // New: vehicle departure via RAFT consensus
    };

    struct PassCommandEntry {
        int vehicleId;
        simtime_t proposedTime;
    };
    
    struct VehicleLeftEntry {
        int vehicleId;
        int batch;          // Which batch this vehicle was in
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
        // P1 Additions:
        int blockedByVehicleId;  // ID of vehicle blocking this one (-1 if none)
        double waitingTimeMs;    // How long this vehicle has been waiting
        double distanceToJunction; // Distance to junction center (meters)
    };
    
    // Legacy structs (kept for compatibility during transition)
    struct VehicleStatus {
        int vehicleId;
        bool wayOfSight;
        char lane[64];
        int positionInLane;
        int direction; // 0=Straight, 1=Left, 2=Right
    };
    
    struct StatusReportEntry {
        int numVehicles;
        VehicleStatus statuses[32];
    };
    
    // PassSchedule: batched pass order with parallel non-conflicting movements
    struct PassBatch {
        int numVehicles;
        int vehicleIds[8];   // Vehicles that can pass simultaneously (max 8 per batch)
    };
    
    struct PassScheduleEntry {
        int numBatches;
        PassBatch batches[16]; // Up to 16 sequential batches (supports up to 64 vehicles)
    };
    
    // Legacy PassOrderEntry (flat order, kept for log compatibility)
    struct PassOrderEntry {
        int numVehicles;
        int order[32];
    };

    std::map<raft_index_t, PassCommandEntry> pendingProposals_;
    raft_index_t lastAppliedIndex_;
    
    std::map<int, VehicleProposal> collectedProposals_;
    std::map<int, VehicleStatus> committedStatuses_;
    PassOrderEntry committedPassOrder_;
    int waitingForVehicle_;
    bool hasCommittedOrder_;
    
    // Fair scheduling state
    int currentBatch_;                   // Which batch is currently passing
    int myBatch_;                        // The batch this vehicle is assigned to cross in
    PassScheduleEntry committedSchedule_;
    std::set<int> vehiclesLeftInBatch_;  // Track which vehicles left in current batch

    // ============ FALLBACK STATE ============
    bool isFallbackMode_;
    int failedElectionCount_;
    raft_term_t lastCheckedTerm_;
    
    // ============ CONFIGURABLE PARAMETERS ============
    int totalVehicles_;
    int electionTimeoutBaseMs_;
    int electionTimeoutJitterMs_;
    int requestTimeoutMs_;
    int maxFailedElections_;
    int fallbackWaitMinMs_;
    int fallbackWaitMaxMs_;
    int passConfirmationMs_;
    int statusCollectionTimeoutMs_;
    std::string resultsFileName_;
    
    static constexpr double CHECK_INTERVAL = 0.1;
    static constexpr double RAFT_PERIODIC_INTERVAL = 0.1;
    
    // ============ TIMERS ============
    cMessage* checkTimer_;
    cMessage* raftPeriodicTimer_;
    cMessage* statusTimeoutTimer_;
    cMessage* passOrderTimer_;
    cMessage* fallbackTimer_;
    cMessage* arrivalWaitTimer_;       // Timer to wait for all vehicles to arrive
    bool waitingForVehiclesToArrive_;  // Flag for arrival wait state
    
    // ============ METRICS TRACKING ============
    simtime_t timeArrived_;
    simtime_t timeStopped_;
    simtime_t timeClusterFormed_;   // When RAFT cluster was formed
    simtime_t timeElected_;
    simtime_t timeOrderCommitted_;
    simtime_t timeStartedMoving_;
    simtime_t timePassed_;
    simtime_t timeVirtualPassed_;
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
    static bool isGlobalInitialized_;
    
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
    
    // ============ WAVE MESSAGE SENDING ============
    void sendRaftMessage(int msgType, int targetId, const std::vector<uint8_t>& data);
    void broadcastRaftMessage(int msgType, const std::vector<uint8_t>& data);
    
    // ============ RAFT MESSAGE HANDLING ============
    void handleRequestVote(const std::vector<uint8_t>& data, int senderId);
    void handleRequestVoteResponse(const std::vector<uint8_t>& data, int senderId);
    void handleAppendEntries(const std::vector<uint8_t>& data, int senderId);
    void handleAppendEntriesResponse(const std::vector<uint8_t>& data, int senderId);
    
    std::vector<uint8_t> serializeRequestVote(msg_requestvote_t* msg);
    std::vector<uint8_t> serializeRequestVoteResponse(msg_requestvote_response_t* msg);
    std::vector<uint8_t> serializeAppendEntries(msg_appendentries_t* msg);
    std::vector<uint8_t> serializeAppendEntriesResponse(msg_appendentries_response_t* msg);
    
    void deserializeRequestVote(const std::vector<uint8_t>& data, msg_requestvote_t* msg);
    void deserializeRequestVoteResponse(const std::vector<uint8_t>& data, msg_requestvote_response_t* msg);
    void deserializeAppendEntries(const std::vector<uint8_t>& data, msg_appendentries_t* msg);
    void deserializeAppendEntriesResponse(const std::vector<uint8_t>& data, msg_appendentries_response_t* msg);
    
    // ============ COORDINATION PROTOCOL ============
    void sendStatusRequest();
    void handleStatusRequest(int fromLeader);
    void sendStatusResponse(int toLeader);
    void handleStatusResponse(int fromVehicle, const std::vector<uint8_t>& payload);
    void sendVehiclePassed();
    void handleVehiclePassed(int vehicleId);
    
    void collectStatusAndDecide();
    void proposeStatusReport();
    void proposePassOrder();
    void executePassOrder();
    int getLaneIndex(const std::string& lane);
    
    // Fair scheduling
    bool conflictsWithBatch(const VehicleProposal& proposal, const std::vector<VehicleProposal>& batch);
    bool movementsConflict(int laneA, int turnA, int laneB, int turnB);
    VehicleProposal buildMyProposal();
    bool amIFirstInLane();
    
    // P1: Blocked vehicle detection
    int detectBlockingVehicle();  // Returns ID of vehicle blocking me, or -1
    void updateBlockedStatus();   // Called when a vehicle leaves to update blocked status
    double calculateDistanceToJunction();  // Calculate distance to junction center
    
    int selectVehicleToPass();
    void proposePassCommand(int vehicleId);
    void executePassCommand(int vehicleId);
    
    void sendVehicleLeft();
    void handleVehicleLeft(int vehicleId);
    void proposeVehicleLeft(int vehicleId, int batch);  // RAFT-based vehicle departure
    void applyVehicleLeftFromRaft(int vehicleId, int batch);  // Apply committed VEHICLE_LEFT
    void rebroadcastVehicleLeft(int vehicleId);
    
    void calculateWayOfSight();
    
    // ============ INTERSECTION COORDINATION ============
    void checkAndStopAtIntersection();
    bool isAtIntersection() const;
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
