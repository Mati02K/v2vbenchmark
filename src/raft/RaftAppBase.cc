// RaftAppBase.cc — Constructor, destructor, and parameter parsing only.
// All protocol logic has been split into focused files:
//   RaftDiscovery.cc   — membership discovery, cluster invitation, formation/merging
//   RaftCore.cc        — RAFT election, log replication, serialization, proposal submission
//   RaftDecision.cc    — crossing-order scheduling algorithm (edit to change pass policy)
//   RaftCoordination.cc — follower-side handlers (status response, vehicle-passed/left)
//   RaftExit.cc        — exit notifications, batch advancement, metrics output
//   RaftUtilities.cc   — intersection detection, movement control, fallback, gossip relay

#include "raft/RaftAppBase.h"
#include "raft/RaftLogger.h"

#include <cstring>

#define NOW (simTime())

// ============ CONSTRUCTOR / DESTRUCTOR ============

RaftAppBase::RaftAppBase()
    : clusterPhase_(PHASE_DISCOVERY)
    , raftServer_(nullptr)
    , myId_(0), myRaftNodeId_(0), myLaneIndex_(0)
    , isLaneLeader_(false)
    , raftStarted_(false)
    , discoveryBeaconInterval_(0.1)
    , clusterTriggerDistance_(200.0)
    , isLeader_(false)
    , hasStoppedAtIntersection_(false)
    , hasPassedIntersection_(false)
    , wayOfSight_(false)
    , vehicleInFrontOfMe_(-1)
    , waitingForStatus_(false)
    , statusResponseCount_(0)
    , lastAppliedIndex_(0)
    , hasCommittedOrder_(false)
    , currentBatch_(0)
    , myBatch_(-1)
    , passOrderProposed_(false)
    , roundNumber_(1)
    , seekingNewCluster_(false)
    , qcAssembled_(false)
    , hasPrevRoundQC_(false)
    , isFallbackMode_(false)
    , failedElectionCount_(0)
    , lastCheckedTerm_(0)
    , totalVehicles_(4)
    , electionTimeoutBaseMs_(500)
    , electionTimeoutJitterMs_(150)
    , requestTimeoutMs_(30)
    , intersectionStopDistance_(15.0)
    , arrivalWaitTimeMs_(1000)
    , maxFailedElections_(4)
    , fallbackWaitMinMs_(2000)
    , fallbackWaitMaxMs_(4000)
    , passConfirmationMs_(200)
    , statusCollectionTimeoutMs_(3000)
    , fallbackClusterTimeoutMs_(30000)
    , discoveryWaitMs_(3000)
    , clusterFormationDelayMs_(0)
    , mergeCooldownMs_(2000)
    , vehicleLeftTimeoutMs_(1500)
    , invitationIntervalStoppedMs_(10)
    , transportName_("unknown")
    , isPriorityVehicle_(false)
    , myPrivKey_(nullptr)
    , timeArrived_(SIMTIME_ZERO)
    , timeStopped_(SIMTIME_ZERO)
    , timeClusterFormed_(SIMTIME_ZERO)
    , timeElected_(SIMTIME_ZERO)
    , timeOrderCommitted_(SIMTIME_ZERO)
    , timeStartedMoving_(SIMTIME_ZERO)
    , timePassed_(SIMTIME_ZERO)
    , totalRaftDecisionTimeSec_(0.0)
    , timeStatusRequestSent_(SIMTIME_ZERO)
    , statusCollectionTimeMs_(0.0)
    , lastRaftPeriodicRun_(SIMTIME_ZERO)
    , lastMergeTime_(SIMTIME_ZERO)
    , waitingForVehiclesToArrive_(false)
    , messagesSent_(0)
    , messagesReceived_(0)
    , electionRounds_(0)
    , wasElectedLeader_(false)
    , coordinationMethod_("raft")
    , logEntriesProposed_(0)
    , logEntriesCommitted_(0)
    , metricsWritten_(false)
    , lastStopDebugPrint_(SIMTIME_ZERO)
    , lastQueueDebugPrint_(SIMTIME_ZERO)
    , prevRoadId_("")
{
    memset(&prevRoundQC_, 0, sizeof(prevRoundQC_));
}

RaftAppBase::~RaftAppBase()
{
    if (raftServer_) {
        raft_free(raftServer_);
        raftServer_ = nullptr;
    }
    if (myPrivKey_) {
        EVP_PKEY_free(myPrivKey_);
        myPrivKey_ = nullptr;
    }
}

// ============ PARAMETER PARSING ============

void RaftAppBase::parseEdgeParameters()
{
    // Approach edges (intersection edges — 4 lanes, one per compass direction)
    // Both subclasses read from the OMNeT++ parameter system before calling this,
    // so approachEdgeList_ and exitEdgeList_ are already filled in.

    for (const auto& e : approachEdgeList_) {
        intersectionEdges_.insert(e);
    }
    for (const auto& e : exitEdgeList_) {
        exitEdges_.insert(e);
    }
}
