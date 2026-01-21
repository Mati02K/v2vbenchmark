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
 * GREEDY RAFT-based intersection coordination
 * 
 * GREEDY ALGORITHM:
 * - Leader prioritizes vehicles in their OWN LANE first
 * - If leader is at back of lane, commands front vehicle(s) to pass first
 * - Once entire lane is cleared, leader passes
 * - Then new election for remaining vehicles
 * - This is "greedy" because leader helps their lane before others
 */
class GreedyRaftApplication : public veins::VeinsInetApplicationBase
{
public:
    GreedyRaftApplication();
    virtual ~GreedyRaftApplication();

protected:
    virtual bool startApplication() override;
    virtual bool stopApplication() override;
    virtual void processPacket(std::shared_ptr<inet::Packet> pk) override;

private:
    // ============ RAFT CORE ============
    raft_server_t* raftServer_;
    
    // Vehicle info
    int myId_;
    int myRaftNodeId_;
    std::string myRoute_;
    std::string myLane_;
    int mySideIndex_;    // Which direction (0=N, 1=S, 2=E, 3=W)
    int myPositionInLane_;  // Position in lane (0=front, 1=back, etc.)
    
    // Mobility
    veins::VeinsInetMobility* mobility_;
    veins::TraCICommandInterface* traci_;
    veins::TraCICommandInterface::Vehicle* traciVehicle_;
    
    // ============ INTERSECTION STATE ============
    bool isLeader_;
    bool hasStoppedAtIntersection_;
    bool hasPassedIntersection_;
    std::string intersectionEdge_;
    
    // wayOfSight: true if no vehicle in front on same lane
    bool wayOfSight_;
    
    // Track which vehicles are still at intersection (not passed)
    std::set<int> activeVehicles_;
    
    // Track received wayOfSight statuses (leader only)
    std::map<int, bool> collectedWayOfSight_;
    bool waitingForStatus_;
    int statusResponseCount_;
    
    // Track who is in front of me on my lane
    int vehicleInFrontOfMe_;
    
    // Intersection edges
    static const std::set<std::string> INTERSECTION_EDGES;
    static const std::map<std::string, std::string> ROUTE_TO_LANE;
    
    // ============ FALLBACK STATE ============
    bool isFallbackMode_;
    int failedElectionCount_;
    raft_term_t lastCheckedTerm_;
    
    // ============ CONFIGURABLE PARAMETERS ============
    int totalVehicles_;
    int vehiclesPerSide_;
    int electionTimeoutBaseMs_;
    int electionTimeoutJitterMs_;
    int requestTimeoutMs_;
    int maxFailedElections_;
    int fallbackWaitMinMs_;
    int fallbackWaitMaxMs_;
    int passConfirmationMs_;
    int statusCollectionTimeoutMs_;
    std::string resultsFileName_;
    
    // Fixed intervals
    static constexpr double CHECK_INTERVAL = 0.05;
    static constexpr double RAFT_PERIODIC_INTERVAL = 0.02;
    
    // ============ METRICS TRACKING ============
    simtime_t timeArrived_;
    simtime_t timeStopped_;
    simtime_t timeElected_;
    simtime_t timeStartedMoving_;
    simtime_t timePassed_;
    int messagesSent_;
    int messagesReceived_;
    int electionRounds_;
    bool wasElectedLeader_;
    std::string coordinationMethod_;
    
    // Static results management
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
    
    // ============ RAFT MESSAGE HANDLING ============
    void handleRequestVote(const std::vector<uint8_t>& data, const std::string& packetName);
    void handleRequestVoteResponse(const std::vector<uint8_t>& data, const std::string& packetName);
    void handleAppendEntries(const std::vector<uint8_t>& data, const std::string& packetName);
    void handleAppendEntriesResponse(const std::vector<uint8_t>& data, const std::string& packetName);
    
    // Packet parsing
    int extractTargetFromPacketName(const std::string& packetName);
    int extractSenderFromPacketName(const std::string& packetName);
    
    // Serialization
    std::vector<uint8_t> serializeRequestVote(msg_requestvote_t* msg);
    std::vector<uint8_t> serializeRequestVoteResponse(msg_requestvote_response_t* msg);
    std::vector<uint8_t> serializeAppendEntries(msg_appendentries_t* msg);
    std::vector<uint8_t> serializeAppendEntriesResponse(msg_appendentries_response_t* msg);
    
    void deserializeRequestVote(const std::vector<uint8_t>& data, msg_requestvote_t* msg);
    void deserializeRequestVoteResponse(const std::vector<uint8_t>& data, msg_requestvote_response_t* msg);
    void deserializeAppendEntries(const std::vector<uint8_t>& data, msg_appendentries_t* msg);
    void deserializeAppendEntriesResponse(const std::vector<uint8_t>& data, msg_appendentries_response_t* msg);
    
    // ============ GREEDY COORDINATION PROTOCOL ============
    
    void sendStatusRequest();
    void handleStatusRequest(int fromLeader);
    void sendStatusResponse(int toLeader, bool wayOfSight);
    void handleStatusResponse(int fromVehicle, bool wayOfSight);
    void sendPassCommand(int toVehicle);
    void handlePassCommand(int fromLeader);
    void sendVehiclePassed();
    void handleVehiclePassed(int vehicleId);
    
    // GREEDY Leader decision making - prioritize own lane
    void collectStatusAndDecideGreedy();
    int selectVehicleToPassGreedy();
    
    // Lane helpers
    int getLaneStartId(int vehicleId) const;
    int getSideIndex(int vehicleId) const;
    int getPositionInLane(int vehicleId) const;
    std::vector<int> getLaneMates(int vehicleId) const;
    bool isVehicleInMyLane(int vehicleId) const;
    bool canVehiclePass(int vehicleId) const;
    
    // wayOfSight calculation
    void calculateWayOfSight();
    void updateWayOfSightAfterPass(int passedVehicleId);
    
    // ============ INTERSECTION COORDINATION ============
    void checkAndStopAtIntersection();
    bool isAtIntersection() const;
    bool hasPassedIntersectionEdge() const;
    void onBecameLeader();
    void onLostLeadership();
    void resumeMovement();
    void stopVehicle();
    void handleFallback();
    
    // RAFT periodic
    void processRaftPeriodic();
    
    // Metrics output
    void outputMetricsJSON();
    static void openResultsFile(const std::string& filename);
    static void closeResultsFile();
    
    // Utility
    raft_node_id_t getNodeIdFromVehicleId(int vehicleId) const;
    int getVehicleIdFromNodeId(raft_node_id_t nodeId) const;
    void sendCustomMessage(const std::string& type, const std::string& data);
};
