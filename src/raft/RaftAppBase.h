#pragma once
// RaftAppBase.h — Abstract base class for RAFT-based intersection coordination
// Contains all shared state and logic; subclasses provide only transport glue.
//
// Derived:
//   WillemtRaftApplication     — UDP (INET) transport
//   WillemtRaftWaveApplication — WAVE (Veins 802.11p) transport

#include "raft/RaftShared.h"
#include "raft/RaftMetrics.h"
#include "veins/modules/mobility/traci/TraCICommandInterface.h"

#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

extern "C" {
#include "../../third_party/raft/raft_types.h"
#include "../../third_party/raft/raft.h"
}

class RaftAppBase
{
public:
    RaftAppBase();
    virtual ~RaftAppBase();

protected:
    // ============ CLUSTER PHASES ============
    enum ClusterPhase {
        PHASE_DISCOVERY,    // Broadcasting/receiving beacons
        PHASE_FORMATION,    // Cluster forming, RAFT initialising
        PHASE_COORDINATION, // RAFT running, scheduling passes
        PHASE_PASSED        // Vehicle has exited intersection
    };
    ClusterPhase clusterPhase_;

    // ============ RAFT CORE ============
    raft_server_t* raftServer_;

    // Vehicle identity
    int         myId_;
    int         myRaftNodeId_;
    int         myLaneIndex_;
    std::string myRoute_;
    std::string myLane_;

    // TraCI (set by subclass after mobility is available)
    veins::TraCICommandInterface*          traci_          = nullptr;
    veins::TraCICommandInterface::Vehicle* traciVehicle_   = nullptr;

    // ============ DISCOVERY STATE ============
    std::set<int> discoveredPeers_;
    double        discoveryBeaconInterval_;
    double        clusterTriggerDistance_;
    bool          clusterFormed_;
    bool          clusterFormationScheduled_;  // guard: don't schedule formation twice

    // ============ INTERSECTION EDGES ============
    std::set<std::string>    intersectionEdges_;
    std::vector<std::string> approachEdgeList_;
    std::set<std::string>    exitEdges_;
    std::vector<std::string> exitEdgeList_;

    // ============ INTERSECTION STATE ============
    bool        isLeader_;
    bool        hasStoppedAtIntersection_;
    bool        hasPassedIntersection_;
    std::string intersectionEdge_;
    bool        wayOfSight_;
    int         vehicleInFrontOfMe_;
    std::set<int>      activeVehicles_;
    std::map<int,bool> collectedWayOfSight_;
    bool        waitingForStatus_;
    int         statusResponseCount_;

    // ============ RAFT LOG STATE ============
    raft_index_t            lastAppliedIndex_;
    std::map<int,VehicleStatus>    committedStatuses_;
    std::map<int,VehicleProposal>  collectedProposals_;
    PassOrderEntry                 committedPassOrder_;
    PassScheduleEntry              committedSchedule_;
    int          waitingForVehicle_;
    bool         hasCommittedOrder_;

    // Batch execution
    int          currentBatch_;
    int          myBatch_;
    std::set<int> vehiclesLeftInBatch_;
    std::set<int> proposedLeft_;      // tracks vehicles proposed-left to RAFT (per-instance)
    bool         passOrderProposed_;  // guard: prevent double proposePassOrder() calls

    // ============ FALLBACK STATE ============
    bool         isFallbackMode_;
    int          failedElectionCount_;
    raft_term_t  lastCheckedTerm_;

    // ============ CONFIGURABLE PARAMETERS ============
    int    totalVehicles_;
    int    electionTimeoutBaseMs_;
    int    electionTimeoutJitterMs_;
    int    requestTimeoutMs_;
    double intersectionStopDistance_;
    int    arrivalWaitTimeMs_;
    int    maxFailedElections_;
    int    fallbackWaitMinMs_;
    int    fallbackWaitMaxMs_;
    int    passConfirmationMs_;
    int    statusCollectionTimeoutMs_;
    std::string resultsFileName_;
    std::string transportName_;  // "udp" or "wave"

    static constexpr double CHECK_INTERVAL       = 0.05;
    static constexpr double RAFT_PERIODIC_INTERVAL = 0.02;

    // ============ METRICS ============
    simtime_t timeArrived_;
    simtime_t timeStopped_;
    simtime_t timeClusterFormed_;
    simtime_t timeElected_;
    simtime_t timeOrderCommitted_;
    simtime_t timeStartedMoving_;
    simtime_t timePassed_;
    bool      waitingForVehiclesToArrive_;
    int       messagesSent_;
    int       messagesReceived_;
    int       electionRounds_;
    bool      wasElectedLeader_;
    std::string coordinationMethod_;
    int       logEntriesProposed_;
    int       logEntriesCommitted_;
    bool      metricsWritten_;
    simtime_t lastRaftPeriodicRun_;

    // ============ PURE-VIRTUAL TRANSPORT INTERFACE (3 methods only) ============
    // Subclasses implement these; everything else is in the base.

    /** Send a unicast RAFT message to a specific vehicle. */
    virtual void sendRaftUnicast(int targetVehicleId,
                                 int msgType,
                                 const std::vector<uint8_t>& data) = 0;

    /** Broadcast a RAFT message to all vehicles. */
    virtual void sendRaftBroadcast(int msgType,
                                   const std::vector<uint8_t>& data) = 0;

    /** Return distance to junction centre (m). Returns large value when unknown. */
    virtual double getDistanceToJunction() const = 0;

    // ============ SHARED METHODS (implemented in RaftAppBase.cc) ============

    // Initialisation helpers
    void parseEdgeParameters();
    void initVehicleIdentity();  // Sets myLane_, myRoute_, wayOfSight_, activeVehicles_

    // Cluster formation
    void sendDiscoveryBeacon();
    void handleDiscoveryBeacon(int senderId, uint8_t senderPhase);
    void checkClusterTrigger();
    void broadcastClusterForm();
    void handleClusterForm(const std::vector<uint8_t>& data, int senderId);
    void handleClusterExists(const std::vector<uint8_t>& data, int senderId);
    void mergeIntoCluster(const std::set<int>& members);
    void broadcastClusterExists();
    void formCluster(const std::set<int>& members);

    // RAFT periodic + callbacks
    void processRaftPeriodic();
    static int  sendRequestVote(raft_server_t*, void*, raft_node_t*, msg_requestvote_t*);
    static int  sendAppendEntries(raft_server_t*, void*, raft_node_t*, msg_appendentries_t*);
    static int  logOffer(raft_server_t*, void*, raft_entry_t*, raft_index_t);
    static int  applylog(raft_server_t*, void*, raft_entry_t*, raft_index_t);
    static void raftLog(raft_server_t*, raft_node_t*, void*, const char*);
    static int  persistVote(raft_server_t*, void*, raft_node_id_t);
    int  doSendRequestVote(raft_node_t*, msg_requestvote_t*);
    int  doSendAppendEntries(raft_node_t*, msg_appendentries_t*);
    int  doLogOffer(raft_entry_t*, raft_index_t);
    int  doApplyLog(raft_entry_t*, raft_index_t);

    // Serialization (all 4 pairs — 100% identical in both originals)
    std::vector<uint8_t> serializeRequestVote(msg_requestvote_t*);
    std::vector<uint8_t> serializeRequestVoteResponse(msg_requestvote_response_t*);
    std::vector<uint8_t> serializeAppendEntries(msg_appendentries_t*);
    std::vector<uint8_t> serializeAppendEntriesResponse(msg_appendentries_response_t*);
    void deserializeRequestVote(const std::vector<uint8_t>&, msg_requestvote_t*);
    void deserializeRequestVoteResponse(const std::vector<uint8_t>&, msg_requestvote_response_t*);
    void deserializeAppendEntries(const std::vector<uint8_t>&, msg_appendentries_t*);
    void deserializeAppendEntriesResponse(const std::vector<uint8_t>&, msg_appendentries_response_t*);

    // Coordination protocol
    void sendStatusRequest();
    void handleStatusRequest(int fromLeader);
    void sendStatusResponse(int toLeader);
    void handleStatusResponse(int fromVehicle, bool wos);
    void handleStatusResponseProposal(int fromVehicle, const VehicleProposal& proposal);
    void collectStatusAndDecide();
    void proposeStatusReport();
    void proposePassOrder();
    void executePassOrder();
    void applyCommittedPassOrder();
    void sendVehiclePassed();
    void handleVehiclePassed(int vehicleId);
    void sendVehicleLeft();
    void handleVehicleLeft(int vehicleId);
    void proposeVehicleLeft(int vehicleId, int batchId);
    void applyVehicleLeftFromRaft(int vehicleId, int batchId);
    void checkBatchAdvance();

    // Scheduling helpers
    bool          movementsConflict(int laneA, int turnA, int laneB, int turnB);
    VehicleProposal buildMyProposal();
    int           detectBlockingVehicle();
    double        calculateDistanceToJunction();
    int           getLaneIndex(const std::string& lane);

    // Intersection state helpers
    void checkAndStopAtIntersection();
    void checkAndAdvanceInQueue();   // TraCI-based: advance queued vehicle when gap opens
    bool isAtIntersection() const;
    bool isNearJunction() const;
    bool hasPassedIntersectionEdge() const;
    void calculateWayOfSight();
    void checkIfLeftIntersection();
    void stopVehicle();
    void resumeMovement();
    void onBecameLeader();
    void onLostLeadership();
    void handleFallback();

    // Metrics
    void outputMetricsJSON();

    // Node ID mapping
    raft_node_id_t getNodeIdFromVehicleId(int v) const  { return v + 1; }
    int            getVehicleIdFromNodeId(raft_node_id_t n) const { return n - 1; }

    // ============ TRANSPORT HELPERS (must be overridden by subclass) ============
    // Called by shared methods when they need to schedule a delayed callback.
    virtual void scheduleOneshotMs(double delayMs, std::function<void()> fn) = 0;

    /** Hook called when the RAFT cluster has been successfully formed */
    virtual void onClusterFormed() = 0;

    /** Return a uniform random double in [lo, hi). Must use the OMNeT++ RNG
     *  so that --seed-set properly controls randomness across runs. */
    virtual double getRandomDouble(double lo, double hi) = 0;
};
