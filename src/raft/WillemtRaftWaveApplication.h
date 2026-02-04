#pragma once

#include "veins/modules/application/ieee80211p/DemoBaseApplLayer.h"
#include "veins/modules/mobility/traci/TraCIMobility.h"
#include "veins/modules/mobility/traci/TraCICommandInterface.h"
#include <fstream>
#include <map>
#include <set>

extern "C" {
#include "../../third_party/raft/raft_types.h"
#include "../../third_party/raft/raft.h"
}

using namespace veins;

/**
 * RAFT-based intersection coordination using willemt/raft library
 * WAVE/802.11p transport version
 * 
 * ALGORITHM WITH QUORUM CONSENSUS:
 * - Leader collects wayOfSight from all vehicles
 * - Leader picks one vehicle with wayOfSight=true to pass
 * - Leader PROPOSES this decision to Raft as a log entry
 * - Only when entry is COMMITTED (replicated to quorum), vehicle passes
 * - If leader fails before commit, new leader won't execute uncommitted entries
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
    // ============ RAFT CORE ============
    raft_server_t* raftServer_;
    
    // Vehicle info
    int myId_;
    int myRaftNodeId_;
    std::string myRoute_;
    std::string myLane_;
    
    // Mobility
    TraCIMobility* mobility_;
    TraCICommandInterface* traci_;
    TraCICommandInterface::Vehicle* traciVehicle_;
    
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
    
    static const std::set<std::string> INTERSECTION_EDGES;
    static const std::map<std::string, std::string> ROUTE_TO_LANE;
    
    // ============ RAFT LOG ENTRY TYPES ============
    enum LogEntryType : uint8_t {
        PASS_COMMAND = 1,
        STATUS_REPORT = 2,
        PASS_ORDER = 3
    };

    struct PassCommandEntry {
        int vehicleId;
        simtime_t proposedTime;
    };
    
    struct VehicleStatus {
        int vehicleId;
        bool wayOfSight;
        char lane[8];
        int positionInLane;
        int direction; // 0=Straight, 1=Left, 2=Right
    };
    
    struct StatusReportEntry {
        int numVehicles;
        VehicleStatus statuses[32];
    };
    
    struct PassOrderEntry {
        int numVehicles;
        int order[32];
    };

    std::map<raft_index_t, PassCommandEntry> pendingProposals_;
    raft_index_t lastAppliedIndex_;
    
    std::map<int, VehicleStatus> committedStatuses_;
    PassOrderEntry committedPassOrder_;
    int waitingForVehicle_;
    bool hasCommittedOrder_;

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
    
    // ============ METRICS TRACKING ============
    simtime_t timeArrived_;
    simtime_t timeStopped_;
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
    
    // ============ COORDINATION PROTOCOL WITH QUORUM ============
    void sendStatusRequest();
    void handleStatusRequest(int fromLeader);
    void sendStatusResponse(int toLeader, bool wayOfSight, int posInLane, int direction);
    void handleStatusResponse(int fromVehicle, bool wayOfSight, int posInLane, int direction);
    void sendVehiclePassed();
    void handleVehiclePassed(int vehicleId);
    
    void collectStatusAndDecide();
    void proposeStatusReport();
    void proposePassOrder();
    std::vector<int> createDeterministicOrder();
    void executePassOrder();
    void checkIfPreviousVehicleLeft(int previousVehicleId);
    int findMyPositionInOrder();
    int getLaneIndex(const std::string& lane);
    
    int selectVehicleToPass();
    void proposePassCommand(int vehicleId);
    void executePassCommand(int vehicleId);
    
    void sendVehicleLeft();
    void handleVehicleLeft(int vehicleId);
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
