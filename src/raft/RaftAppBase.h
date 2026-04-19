#pragma once
// RaftAppBase.h — Abstract base class for RAFT-based intersection coordination
// Contains all shared state and logic; subclasses provide only transport glue.
//
// Derived:
//   UdpRaftApplication   — UDP (INET) transport
//   WaveRaftApplication  — WAVE (Veins 802.11p) transport

#include "raft/RaftShared.h"
#include "raft/RaftMetrics.h"
#include "../../lib/crypto/CryptoAuth.h"
#include <openssl/evp.h>
#include "veins/modules/mobility/traci/TraCICommandInterface.h"

#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

extern "C" {
#include "../../lib/raft/raft_types.h"
#include "../../lib/raft/raft.h"
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

    // Cached proposal — static fields set once in initMyProposal(), dynamic fields
    // refreshed on each updateMyProposal() call.
    VehicleProposal myProposal_;

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

    // collectedLaneLeaders_: lane leaders heard during cluster formation.
    // Key = laneIndex, Value = vehicleId of that lane's leader.
    // Populated by handleClusterJoinInvite(); once size == numLanes → formCluster().
    std::map<int, int> collectedLaneLeaders_;

    // collectedLeaderDBs_: full vehicleDB_ received from each lane leader after election.
    // Key = senderVehicleId, Value = that leader's full vehicleDB_.
    // Built during STATUS_REQUEST phase; used by proposePassOrder() instead of stale local vehicleDB_.
    std::map<int, std::map<int, VehicleProposal>> collectedLeaderDBs_;

    // raftStarted_: true once formCluster() has been called. Guards against double-formation.
    bool raftStarted_;

    double        discoveryBeaconInterval_;
    double        clusterTriggerDistance_;  // kept for NED param compat, not used for formation

    // ============ CLUSTER MODE ============
    // "laneLeaders" = only lane leaders join RAFT (default, current behavior)
    // "allVehicles" = every vehicle joins RAFT cluster
    std::string   clusterMode_;

    // collectedAllVehicles_: all vehicles that announced readiness (for allVehicles mode).
    // In laneLeaders mode, collectedLaneLeaders_ (keyed by lane) is used instead.
    std::set<int> collectedAllVehicles_;

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
    bool        waitingForStatus_;
    int         statusResponseCount_;

    // ============ RAFT LOG STATE ============
    raft_index_t            lastAppliedIndex_;
    PassScheduleEntry       committedSchedule_;
    PassScheduleEntry       pendingSchedule_;    // schedule held between QC signing and RAFT submission
    bool         hasCommittedOrder_;

    // Batch execution
    int          currentBatch_;
    int          myBatch_;
    std::set<int> vehiclesLeftInBatch_;
    std::set<int> proposedLeft_;      // dedup guard: prevents double-proposing the same vehicle exit to RAFT
    bool         passOrderProposed_;  // guard: prevent double proposePassOrder() calls

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
    int              qcRetryCount_;              // how many times we've retried QC signing

    // ---- QC storage (all vehicles) ----
    // Stored when QC_BROADCAST is received (or when leader assembles the QC).
    // The NEXT round reads hasPrevRoundQC_ + prevRoundQC_ to verify trust.
    bool             hasPrevRoundQC_;
    QuorumCertificate prevRoundQC_;

    // ============ FALLBACK STATE ============
    bool         isFallbackMode_;
    raft_term_t  lastCheckedTerm_;

    // ============ CONFIGURABLE PARAMETERS ============
    int    totalVehicles_;
    int    electionTimeoutBaseMs_;
    int    electionTimeoutJitterMs_;
    int    requestTimeoutMs_;
    double intersectionStopDistance_;
    int    fallbackWaitMinMs_;
    int    fallbackWaitMaxMs_;
    int    passConfirmationMs_;
    int    statusCollectionTimeoutMs_;
    int    fallbackClusterTimeoutMs_;
    int    intersectionStopTimeMs_;
    int    vehicleLeftTimeoutMs_;
    int    qcSignTimeoutMs_;
    int    maxQcRetries_;
    std::string resultsFileName_;
    double      resultsFileCloseAtSec_ = 0;
    std::string transportName_;

    // ============ CRYPTO / AUTHENTICATION ============
    bool         isPriorityVehicle_;                          // set from NED parameter
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

    void parseEdgeParameters();  // RaftAppBase.cc

    // ---- RaftDiscovery.cc: simulation start → cluster formed, vehicle stopped ----
    void sendPeerBeacon();
    void handlePeerBeacon(const std::vector<uint8_t>& data, int senderId);
    void updateLaneLeaderFlag();
    void startNewRound();
    void sendClusterBeacon();
    void tryFormClusterFromCollected();
    void scheduleClusterFormationLoop();
    void handleClusterJoinInvite(const std::vector<uint8_t>& data, int senderId);
    void handleClusterFormBroadcast(const std::vector<uint8_t>& data);
    void formCluster(const std::set<int>& members);
    void checkAndStopAtIntersection();
    void onFirstStoppedAtIntersection();
    void checkAndAdvanceInQueue();
    void updateRoadTracking(const std::string& roadId, const std::string& lastKnownRoad);
    void handleClusterJunctionStop(const std::string& roadId, const std::string& lastKnownRoad);
    void handleFrontStop(const std::string& roadId, double dist);
    void handleQueuedStop(const std::string& roadId, double dist, double speed);

    // ---- RaftCore.cc: election → status collection → propose → QC assembly ----
    void processRaftPeriodic();
    void checkElectionFailures(raft_term_t currentTerm);
    void checkLeadershipChange();
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
    std::vector<uint8_t> serializeRequestVote(msg_requestvote_t*);
    std::vector<uint8_t> serializeRequestVoteResponse(msg_requestvote_response_t*);
    std::vector<uint8_t> serializeAppendEntries(msg_appendentries_t*);
    std::vector<uint8_t> serializeAppendEntriesResponse(msg_appendentries_response_t*);
    void deserializeRequestVote(const std::vector<uint8_t>&, msg_requestvote_t*);
    void deserializeRequestVoteResponse(const std::vector<uint8_t>&, msg_requestvote_response_t*);
    void deserializeAppendEntries(const std::vector<uint8_t>&, msg_appendentries_t*);
    void deserializeAppendEntriesResponse(const std::vector<uint8_t>&, msg_appendentries_response_t*);
    void onBecameLeader();
    void onLostLeadership();
    void sendStatusRequest();
    void handleStatusRequest(int fromLeader);
    void sendDbResponse(int toLeader);
    void handleDbResponse(const std::vector<uint8_t>& data, int senderId);
    void collectStatusAndDecide();
    void proposePassOrder();
    void sendQCSignRequest();
    void handleQCSignRequest(const std::vector<uint8_t>& data);
    void sendQCSignResponse(int toLeader, const std::vector<uint8_t>& respData);
    void handleQCSignResponse(const std::vector<uint8_t>& data, int senderId);
    void tryAssembleQC();
    bool verifyQC(const QuorumCertificate& qc) const;

    // ---- RaftDecision.cc: pure crossing-order algorithm ----
    int               getLaneIndex(const std::string& lane);
    PassScheduleEntry computePassOrder(const std::map<int, VehicleProposal>& proposals,
                                       const std::set<int>& activeVehicles);

public:
    // Public so file-scope helpers in RaftDecision.cc can call it without friend boilerplate.
    static bool movementsConflict(int laneA, int turnA, int laneB, int turnB);

    // ---- RaftCoordination.cc: send QC/schedule to all + execute the pass ----
    void sendPassOrderBroadcast();
    void handlePassOrderBroadcast(const std::vector<uint8_t>& data);
    void applyCommittedPassOrder();
    void handleVehicleLeft(int vehicleId, int batchId);
    void sendVehicleLeft();
    void scheduleVehicleLeftTimeout(int batchIndex);
    void onVehicleLeftTimeout(int batchIndex);
    void checkBatchAdvance();
    void checkIfLeftIntersection();

    // ---- RaftUtilities.cc: shared helpers ----
    bool            verifySignedProposal(const SignedProposal& sp, int senderId) const;
    void            initMyProposal();
    VehicleProposal updateMyProposal();
    int             detectBlockingVehicle();
    double          calculateDistanceToJunction();
    bool            isLaneLeaderByTraci() const;
    void            calculateWayOfSight();
    bool            isAtIntersection() const;
    bool            isNearJunction() const;
    bool            hasPassedIntersectionEdge() const;
    void            stopVehicle();
    void            resumeMovement();
    void            handleFallback();
    bool            isVehicleInCommittedSchedule(int vehicleId) const;
    bool            hasUnscheduledVehicles() const;
    void            markRaftNodeInactive(int vehicleId);
    void            outputMetricsJSON();

    // ---- Node ID mapping (inline) ----
    raft_node_id_t getNodeIdFromVehicleId(int v) const  { return v + 1; }
    int            getVehicleIdFromNodeId(raft_node_id_t n) const { return n - 1; }

    // ============ TRANSPORT HELPERS ============
    virtual void scheduleOneshotMs(double delayMs, std::function<void()> fn) = 0;
    virtual void onClusterFormed() = 0;
    virtual double getRandomDouble(double lo, double hi) = 0;
};
