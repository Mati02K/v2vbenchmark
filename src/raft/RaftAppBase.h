#pragma once
// RaftAppBase.h — Abstract base class for RAFT-based intersection coordination
// Contains all shared state and logic; subclasses provide only transport glue.
//
// Derived:
//   UdpRaftApplication   — UDP (INET) transport
//   WaveRaftApplication  — WAVE (Veins 802.11p) transport

#include "raft/RaftShared.h"
#include "raft/RaftMetrics.h"
#include "raft/CryptoAuth.h"
#include <openssl/evp.h>
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
        PHASE_DISCOVERY,    // Broadcasting/receiving peer beacons, building vehicleDB_
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
    // vehicleDB_: all known vehicles (including self). Built from received PEER_BEACONs.
    // Key = vehicleId, Value = latest VehicleProposal from that vehicle.
    std::map<int, VehicleProposal> vehicleDB_;

    // isLaneLeader_: true if this vehicle has the smallest distanceToJunction in its lane.
    // Recomputed dynamically every check interval from vehicleDB_.
    bool isLaneLeader_;

    // receivedLeaderDBs_: leader DBs received at intersection, key = senderLaneIndex.
    std::map<int, std::map<int, VehicleProposal>> receivedLeaderDBs_;
    // senderVehicleId for each received leader DB, key = laneIndex
    std::map<int, int> receivedLeaderSenderIds_;

    // raftStarted_: true once formCluster() has been called. Guards against double-formation.
    bool raftStarted_;

    double        discoveryBeaconInterval_;
    double        clusterTriggerDistance_;  // kept for NED param compat, not used for formation

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

    // Dedup for VEHICLE_LEFT: prevent double-applying the same vehicle's exit notification
    std::set<int> gossipSeenVehicleLeft_;

    /** Vehicles that passed/left before we formed our cluster. */
    std::set<int> vehiclesLeftBeforeFormed_;

    // ============ MULTI-ROUND STATE ============
    // roundNumber_: starts at 1, incremented each time a new lane leader triggers a
    //   fresh RAFT cluster formation after the previous cluster's PASS_ORDER committed.
    // scheduledVehicles_: union of all vehicles scheduled in earlier rounds.
    //   proposePassOrder() skips these so they are never double-scheduled.
    // seekingNewCluster_: true while waiting for 500ms discovery window to elapse
    //   before initiating leader-DB-exchange for the new round.
    int              roundNumber_;
    std::set<int>    scheduledVehicles_;
    bool             seekingNewCluster_;

    // ---- QC assembly (leader-side) ----
    // After PASS_ORDER commits, the leader collects one ECDSA signature from each
    // cluster member over [uint32_t round || PassScheduleEntry].
    // When majority reached, a QuorumCertificate is broadcast to ALL vehicles.
    struct QCSigEntry {
        uint8_t pubKey[CRYPTO_PUBKEY_BYTES];
        uint8_t sig[CRYPTO_SIG_MAX_BYTES];
        uint8_t sigLen;
    };
    std::map<int, QCSigEntry> collectedQCSigs_;  // vehicleId → signature
    bool             qcAssembled_;               // guards against double-assembly

    // ---- QC storage (all vehicles) ----
    // Stored when QC_BROADCAST is received (or when leader assembles the QC).
    // The NEXT round reads hasPrevRoundQC_ + prevRoundQC_ to verify trust.
    bool             hasPrevRoundQC_;
    QuorumCertificate prevRoundQC_;

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
    int    fallbackClusterTimeoutMs_;
    int    discoveryWaitMs_;
    int    clusterFormationDelayMs_;
    int    mergeCooldownMs_;
    int    vehicleLeftTimeoutMs_;
    int    invitationIntervalStoppedMs_;
    std::string resultsFileName_;
    double      resultsFileCloseAtSec_ = 0;
    std::string transportName_;

    // ============ CRYPTO / AUTHENTICATION ============
    bool         isAmbulance_;                          // set from NED parameter
    EVP_PKEY*    myPrivKey_;                            // vehicle's EC private key
    uint8_t      myPubKey_[CRYPTO_PUBKEY_BYTES];        // vehicle's EC public key
    VehicleCert  myCert_;                               // cert signed by appropriate CA


    static constexpr double CHECK_INTERVAL         = 0.05;
    static constexpr double RAFT_PERIODIC_INTERVAL = 0.02;

    // ============ METRICS ============
    simtime_t timeArrived_;
    simtime_t timeStopped_;
    simtime_t timeClusterFormed_;
    simtime_t timeElected_;
    simtime_t timeOrderCommitted_;
    simtime_t timeStartedMoving_;
    simtime_t timePassed_;

    double totalRaftDecisionTimeSec_;
    std::map<raft_index_t, simtime_t> proposedTimes_;
    simtime_t timeStatusRequestSent_;
    double statusCollectionTimeMs_;
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
    simtime_t lastMergeTime_;
    simtime_t lastStopDebugPrint_;
    simtime_t lastQueueDebugPrint_;
    std::string prevRoadId_;

    // ============ PURE-VIRTUAL TRANSPORT INTERFACE ============
    virtual void sendRaftToPeer(int targetVehicleId,
                                 int msgType,
                                 const std::vector<uint8_t>& data) = 0;
    virtual void sendRaftBroadcast(int msgType,
                                   const std::vector<uint8_t>& data) = 0;
    virtual double getDistanceToJunction() const = 0;

    // ============ SHARED METHODS ============

    void parseEdgeParameters();
    void initVehicleIdentity();

    // Pre-intersection discovery (peer beacons)
    void sendPeerBeacon();
    void handlePeerBeacon(const std::vector<uint8_t>& data, int senderId);

    // Lane leader flag — recomputed from vehicleDB_ every check interval
    void updateLaneLeaderFlag();

    // Intersection phase: leader DB exchange + RAFT cluster formation
    void sendLeaderDbExchange();
    void handleLeaderDbExchange(const std::vector<uint8_t>& data, int senderId);
    void tryFormRaftFromLeaderDBs();
    void sendClusterJoinInvite(const std::set<int>& members);
    void handleClusterJoinInvite(const std::vector<uint8_t>& data, int senderId);
    void scheduleLeaderDbExchangeLoop();

    // Core cluster formation (called once per RAFT lifecycle)
    void formCluster(const std::set<int>& members);

    // RAFT periodic + callbacks
    void processRaftPeriodic();
    static int  sendRequestVote(raft_server_t*, void*, raft_node_t*, msg_requestvote_t*);
    static int  sendAppendEntries(raft_server_t*, void*, raft_node_t*, msg_appendentries_t*);
    static int  logOffer(raft_server_t*, void*, raft_entry_t*, raft_index_t);
    static int  applylog(raft_server_t*, void*, raft_entry_t*, raft_index_t);
    static void raftLog(raft_server_t*, raft_node_t*, void*, const char*);
    static int  persistVote(raft_server_t*, void*, raft_node_id_t);
    static int  logGetNodeId(raft_server_t*, void*, raft_entry_t*, raft_index_t);
    int  doSendRequestVote(raft_node_t*, msg_requestvote_t*);
    int  doSendAppendEntries(raft_node_t*, msg_appendentries_t*);
    int  doLogOffer(raft_entry_t*, raft_index_t);
    int  doApplyLog(raft_entry_t*, raft_index_t);
    int  doLogGetNodeId(raft_entry_t*, raft_index_t);

    // Serialization
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
    void sendPassOrderBroadcast();
    void handlePassOrderBroadcast(const std::vector<uint8_t>& data);
    void sendVehiclePassed();
    void handleVehiclePassed(int vehicleId);
    void sendVehicleLeft();
    void handleVehicleLeft(int vehicleId, int batchId);
    void proposeVehicleLeft(int vehicleId, int batchId);
    void applyVehicleLeftFromRaft(int vehicleId, int batchId);
    void checkBatchAdvance();

    // Scheduling helpers
    bool          movementsConflict(int laneA, int turnA, int laneB, int turnB);
    PassScheduleEntry computePassOrder(const std::map<int, VehicleProposal>& proposals,
                                       const std::set<int>& activeVehicles);
    VehicleProposal buildMyProposal();
    int           detectBlockingVehicle();
    double        calculateDistanceToJunction();
    int           getLaneIndex(const std::string& lane);

    // Intersection state helpers
    void checkAndStopAtIntersection();
    void onFirstStoppedAtIntersection();  // called once when vehicle first stops
    void checkAndAdvanceInQueue();
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

    void markRaftNodeInactive(int vehicleId);
    void scheduleVehicleLeftTimeout(int batchIndex);

    // ---- Multi-round ----
    // Called when isLaneLeader_ transitions false→true while stopped at the intersection
    // with a committed order from the previous round.  Resets RAFT state and re-triggers
    // the leader-DB-exchange loop to form a new cluster for late-arriving vehicles.
    void startNewRound();

    // ---- Quorum Certificate ----
    void sendQCSignRequest();
    void handleQCSignRequest(const std::vector<uint8_t>& data, int senderId);
    void sendQCSignResponse(int toLeader, const std::vector<uint8_t>& tbsData);
    void handleQCSignResponse(const std::vector<uint8_t>& data, int senderId);
    void tryAssembleQC();
    void sendQCBroadcast();
    void handleQCBroadcast(const std::vector<uint8_t>& data);
    bool verifyQC(const QuorumCertificate& qc) const;

    // ============ TRANSPORT HELPERS ============
    virtual void scheduleOneshotMs(double delayMs, std::function<void()> fn) = 0;
    virtual void onClusterFormed() = 0;
    virtual double getRandomDouble(double lo, double hi) = 0;
};
