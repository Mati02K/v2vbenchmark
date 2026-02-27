// RaftAppBase.cc — Shared RAFT intersection logic for both UDP and WAVE transports
// All methods here are transport-agnostic. The 4 pure-virtual methods (sendRaftUnicast,
// sendRaftBroadcast, getDistanceToJunction, scheduleOneshotMs) are implemented by subclasses.

#include "raft/RaftAppBase.h"
#include "raft/RaftLogger.h"
#include "veins/base/utils/FindModule.h"

#include <algorithm>
#include <cstring>
#include <random>
#include <functional>
#include <sstream>

extern "C" {
#include "../../third_party/raft/raft.h"
}

// Convenience macro for simTime()
#define NOW (simTime())

// ============ CONSTRUCTOR / DESTRUCTOR ============

RaftAppBase::RaftAppBase()
    : clusterPhase_(PHASE_DISCOVERY)
    , raftServer_(nullptr)
    , myId_(0), myRaftNodeId_(0), myLaneIndex_(0)
    , discoveryBeaconInterval_(0.1)
    , clusterTriggerDistance_(200.0)
    , clusterFormed_(false)
    , clusterFormationScheduled_(false)
    , isLeader_(false)
    , hasStoppedAtIntersection_(false)
    , hasPassedIntersection_(false)
    , wayOfSight_(false)
    , vehicleInFrontOfMe_(-1)
    , waitingForStatus_(false)
    , statusResponseCount_(0)
    , lastAppliedIndex_(0)
    , waitingForVehicle_(-1)
    , hasCommittedOrder_(false)
    , currentBatch_(0)
    , myBatch_(-1)
    , passOrderProposed_(false)
    , isFallbackMode_(false)
    , failedElectionCount_(0)
    , lastCheckedTerm_(0)
    , totalVehicles_(4)
    , electionTimeoutBaseMs_(500)
    , electionTimeoutJitterMs_(250)
    , requestTimeoutMs_(200)
    , intersectionStopDistance_(15.0)
    , arrivalWaitTimeMs_(2000)
    , maxFailedElections_(5)
    , fallbackWaitMinMs_(1000)
    , fallbackWaitMaxMs_(3000)
    , passConfirmationMs_(200)
    , statusCollectionTimeoutMs_(3000)
    , discoveryWaitMs_(3000)
    , clusterFormationDelayMs_(0)
    , transportName_("unknown")
    , timeArrived_(SIMTIME_ZERO)
    , timeStopped_(SIMTIME_ZERO)
    , timeClusterFormed_(SIMTIME_ZERO)
    , timeElected_(SIMTIME_ZERO)
    , timeOrderCommitted_(SIMTIME_ZERO)
    , timeStartedMoving_(SIMTIME_ZERO)
    , timePassed_(SIMTIME_ZERO)
    , lastRaftPeriodicRun_(SIMTIME_ZERO)
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
{}

RaftAppBase::~RaftAppBase()
{
    if (raftServer_) {
        raft_free(raftServer_);
        raftServer_ = nullptr;
    }
}

// ============ PARAMETER PARSING ============

void RaftAppBase::parseEdgeParameters()
{
    // Approach edges (intersection edges — 4 lanes, one per compass direction)
    // The format is: e.g. "N2C S2C E2C W2C"  or separate params.
    // Both subclasses read from the OMNeT++ parameter system before calling this,
    // so approachEdgeList_ and exitEdgeList_ are already filled in.

    for (const auto& e : approachEdgeList_) {
        intersectionEdges_.insert(e);
    }
    for (const auto& e : exitEdgeList_) {
        exitEdges_.insert(e);
    }
}

// ============ DISCOVERY ============

void RaftAppBase::sendDiscoveryBeacon()
{
    // Payload: [myId(4B)][clusterPhase(1B)]
    std::vector<uint8_t> data(sizeof(int) + 1);
    memcpy(data.data(), &myId_, sizeof(int));
    data[sizeof(int)] = static_cast<uint8_t>(clusterPhase_);
    sendRaftBroadcast(/*DISCOVERY_BEACON*/ 0x10, data);
    messagesSent_++;
}

void RaftAppBase::handleDiscoveryBeacon(int senderId, uint8_t senderPhase)
{
    if (senderId == myId_) return;
    if (clusterPhase_ != PHASE_DISCOVERY) return;
    if (senderId < 0 || senderId >= totalVehicles_) return;  // reject recycled/extra SUMO vehicles
    bool wasNew = (discoveredPeers_.find(senderId) == discoveredPeers_.end());
    discoveredPeers_.insert(senderId);
    activeVehicles_.insert(senderId);
    if (wasNew) {
        std::cout << simTime() << " [DBG][V" << myId_ << "] BEACON from V" << senderId
                  << " => discovered=" << discoveredPeers_.size()+1
                  << "/" << totalVehicles_
                  << " (need " << (totalVehicles_ - (int)(discoveredPeers_.size()+1)) << " more)"
                  << " stopped=" << hasStoppedAtIntersection_ << std::endl;
    }
}

void RaftAppBase::checkClusterTrigger()
{
    if (clusterPhase_ != PHASE_DISCOVERY) return;
    if (!hasStoppedAtIntersection_) return;  // Don't form until stopped at intersection
    if (clusterFormationScheduled_) return;  // already scheduled

    // Use discoveryWaitMs + clusterFormationDelayMs: first to stop waits for (a) peers
    // to arrive and exchange beacons, and (b) an extra settling delay so more vehicles
    // can stop before anyone forms. Total = discoveryWaitMs + clusterFormationDelayMs.
    double requiredWaitMs = static_cast<double>(discoveryWaitMs_ + clusterFormationDelayMs_);
    double waitedMs = (NOW - timeStopped_).dbl() * 1000.0;
    if (waitedMs < requiredWaitMs) return;

    std::set<int> members = discoveredPeers_;
    members.insert(myId_);

    std::cout << simTime() << " [DBG][V" << myId_ << "] CLUSTER_TRIGGER FIRING after "
              << waitedMs << "ms: forming with " << members.size() << "/" << totalVehicles_
              << " peers=[";
    for (int v : members) std::cout << v << " ";
    std::cout << "]" << std::endl;

    clusterFormationScheduled_ = true;
    RAFT_LOG("CLUSTER FORMATION with " << members.size() << "/" << totalVehicles_
             << " vehicles (200 ms discovery window)");
    formCluster(members);
}

// ============ CLUSTER FORMATION ============

void RaftAppBase::formCluster(const std::set<int>& members)
{
    if (clusterPhase_ != PHASE_DISCOVERY) return;

    clusterPhase_ = PHASE_FORMATION;
    clusterFormed_ = true;
    timeClusterFormed_ = NOW;
    activeVehicles_   = members;

    std::cout << simTime() << " [DBG][V" << myId_ << "] FORM_CLUSTER " << members.size() << " members: [";
    for (int v : members) std::cout << v << " ";
    std::cout << "] electionBase=" << electionTimeoutBaseMs_ << "ms" << std::endl;

    // Initialise RAFT server
    raft_server_t* s = raft_new();
    if (!s) {
        RAFT_LOG_ERR("raft_new() returned nullptr!");
        return;
    }
    raftServer_ = s;

    // Set callbacks
    raft_cbs_t cbs;
    memset(&cbs, 0, sizeof(cbs));
    cbs.send_requestvote      = &RaftAppBase::sendRequestVote;
    cbs.send_appendentries    = &RaftAppBase::sendAppendEntries;
    cbs.log_offer             = &RaftAppBase::logOffer;
    cbs.applylog              = &RaftAppBase::applylog;
    cbs.log                   = &RaftAppBase::raftLog;
    cbs.persist_vote          = &RaftAppBase::persistVote;
    raft_set_callbacks(raftServer_, &cbs, this);

    // Randomised election timeout — different each run (seed-set controls the RNG).
    // Using uniform() prevents vehicle 0 from always winning the election.
    int electionTimeout = electionTimeoutBaseMs_ + (int)getRandomDouble(0.0, (double)electionTimeoutJitterMs_);
    int requestTimeout  = requestTimeoutMs_;
    raft_set_election_timeout(raftServer_, electionTimeout);
    raft_set_request_timeout(raftServer_, requestTimeout);

    myRaftNodeId_ = getNodeIdFromVehicleId(myId_);

    // Add all nodes
    for (int vid : members) {
        raft_node_id_t nodeId = getNodeIdFromVehicleId(vid);
        void* udata = reinterpret_cast<void*>(static_cast<intptr_t>(vid));
        raft_node_t* node = raft_add_node(raftServer_, udata, nodeId, (vid == myId_) ? 1 : 0);
        if (!node) {
            RAFT_LOG_ERR("Failed to add Raft node for vehicle " << vid);
        }
    }

    clusterPhase_ = PHASE_COORDINATION;

    // Call virtual hook for transport-specific logic
    onClusterFormed();

    // Broadcast cluster formation to peers
    broadcastClusterForm();

    // Keep broadcasting our cluster membership so that:
    //  (a) vehicles that missed the first packet can still join, and
    //  (b) smaller clusters that formed independently can merge into us.
    // Broadcasts stop once the pass-order is committed (coordination done).
    // Schedule covers ~20 s so late-stopping vehicles (back of 16-vehicle queue)
    // have time to stop, wait their 200 ms, then hear our cluster invite.
    for (int delayMs : {300, 700, 1200, 1800, 3000, 5000, 7000, 10000, 14000, 18000}) {
        scheduleOneshotMs(delayMs, [this]() {
            if (clusterPhase_ == PHASE_COORDINATION && !hasPassedIntersection_ && !hasCommittedOrder_)
                broadcastClusterExists();
        });
    }
}

void RaftAppBase::mergeIntoCluster(const std::set<int>& members)
{
    RAFT_LOG("MERGING INTO existing cluster (" << members.size() << " members)");
    activeVehicles_   = members;
    clusterPhase_     = PHASE_COORDINATION;
    clusterFormed_    = true;
    timeClusterFormed_ = NOW;
}

// Dissolve the current RAFT instance and re-form a larger unified cluster.
// Called when we receive a CLUSTER_INVITE whose member count exceeds ours.
void RaftAppBase::mergeIntoLargerCluster(const std::set<int>& mergedMembers)
{
    if (hasCommittedOrder_ || hasPassedIntersection_) return;

    std::cout << simTime() << " [DBG][V" << myId_
              << "] MERGE_INTO_LARGER: reinitializing with "
              << mergedMembers.size() << " members [";
    for (int v : mergedMembers) std::cout << v << " ";
    std::cout << "]" << std::endl;

    // 1. Free the current RAFT server so we start fresh.
    if (raftServer_) {
        raft_free(raftServer_);
        raftServer_ = nullptr;
    }

    // 2. Reset all RAFT-specific state.
    isLeader_             = false;
    waitingForStatus_     = false;
    statusResponseCount_  = 0;
    passOrderProposed_    = false;
    lastAppliedIndex_     = 0;
    lastCheckedTerm_      = 0;
    failedElectionCount_  = 0;
    lastRaftPeriodicRun_  = SIMTIME_ZERO;
    collectedProposals_.clear();
    collectedWayOfSight_.clear();
    vehiclesLeftInBatch_.clear();
    proposedLeft_.clear();

    // 3. Absorb all merged members into our discovery set.
    for (int v : mergedMembers)
        if (v != myId_) discoveredPeers_.insert(v);

    // 4. Reset phase flags so formCluster() accepts the call.
    clusterPhase_              = PHASE_DISCOVERY;
    clusterFormed_             = false;
    clusterFormationScheduled_ = true;  // prevent duplicate trigger from timer

    // 5. Re-form the cluster with the larger unified member set.
    formCluster(mergedMembers);
}

void RaftAppBase::broadcastClusterForm()
{
    // Payload: [numMembers(1B)][member0(4B)][member1(4B)]...
    std::vector<uint8_t> data;
    uint8_t n = static_cast<uint8_t>(activeVehicles_.size());
    data.push_back(n);
    for (int vid : activeVehicles_) {
        uint8_t bytes[sizeof(int)];
        memcpy(bytes, &vid, sizeof(int));
        data.insert(data.end(), bytes, bytes + sizeof(int));
    }
    sendRaftBroadcast(/*CLUSTER_FORM*/ 0x11, data);
    messagesSent_++;
}

void RaftAppBase::broadcastClusterExists()
{
    broadcastClusterForm();  // Same packet format — receiver checks phase
}

void RaftAppBase::handleClusterForm(const std::vector<uint8_t>& data, int senderId)
{
    if (clusterPhase_ != PHASE_DISCOVERY) {
        // Already in a cluster: run merge-if-bigger logic
        handleClusterExists(data, senderId);
        return;
    }
    if (data.size() < 1) return;

    uint8_t n = data[0];
    if (data.size() < 1u + n * sizeof(int)) return;

    // Absorb all members the sender knows about into our own discovery set.
    // The 200 ms timer in checkClusterTrigger() will fire and actually form
    // the cluster, incorporating all peers gathered here.
    for (int i = 0; i < n; i++) {
        int vid;
        memcpy(&vid, data.data() + 1 + i * sizeof(int), sizeof(int));
        if (vid >= 0 && vid < totalVehicles_ && vid != myId_) {
            bool wasNew = (discoveredPeers_.find(vid) == discoveredPeers_.end());
            discoveredPeers_.insert(vid);
            if (wasNew) {
                std::cout << simTime() << " [DBG][V" << myId_
                          << "] CLUSTER_FORM absorbed peer V" << vid
                          << " from sender V" << senderId
                          << " (now have " << discoveredPeers_.size()+1 << " peers)"
                          << std::endl;
            }
        }
    }
    // checkClusterTrigger() runs on every timer tick and will form the cluster
    // once the 200 ms window has elapsed after stopping.
}

void RaftAppBase::handleClusterExists(const std::vector<uint8_t>& data, int senderId)
{
    if (data.size() < 1) return;

    uint8_t n = data[0];
    if (data.size() < 1u + n * sizeof(int)) return;

    std::set<int> incomingMembers;
    for (int i = 0; i < n; i++) {
        int vid;
        memcpy(&vid, data.data() + 1 + i * sizeof(int), sizeof(int));
        if (vid >= 0 && vid < totalVehicles_)
            incomingMembers.insert(vid);
    }

    // If we're still in DISCOVERY mode, absorb peers and let the timer form the cluster.
    if (clusterPhase_ == PHASE_DISCOVERY) {
        for (int v : incomingMembers) {
            if (v != myId_) discoveredPeers_.insert(v);
        }
        return;
    }

    // ---- We are already in PHASE_COORDINATION ----
    // Build the union of current + incoming members.
    std::set<int> currentMembers = activeVehicles_;
    currentMembers.insert(myId_);

    std::set<int> mergedMembers = currentMembers;
    mergedMembers.insert(incomingMembers.begin(), incomingMembers.end());

    // Merge condition: union adds new vehicles AND no order committed yet.
    // Incoming cluster must be strictly larger; we merge into them.
    bool incomingBigger   = (incomingMembers.size() > currentMembers.size());
    bool unionAddsMembers = (mergedMembers.size()   > currentMembers.size());

    raft_term_t currentTerm = raftServer_ ? raft_get_current_term(raftServer_) : 0;
    bool canMerge         = !hasCommittedOrder_ && !hasPassedIntersection_
                            && lastAppliedIndex_ == 0  // no RAFT progress yet
                            && currentTerm == 0;       // no election started yet

    if (canMerge && unionAddsMembers && incomingBigger) {
        std::cout << simTime() << " [DBG][V" << myId_
                  << "] MERGE: incoming=" << incomingMembers.size()
                  << " mine=" << currentMembers.size()
                  << " -> merging into " << mergedMembers.size() << " members" << std::endl;
        mergeIntoLargerCluster(mergedMembers);
        return;
    }

    // No merge — just update the active-vehicle set with any new arrivals.
    for (int v : incomingMembers)
        activeVehicles_.insert(v);
}

// ============ RAFT PERIODIC ============

void RaftAppBase::processRaftPeriodic()
{
    if (!raftServer_ || isFallbackMode_) return;

    simtime_t now   = NOW;
    if (lastRaftPeriodicRun_ == SIMTIME_ZERO) lastRaftPeriodicRun_ = now;
    simtime_t delta = now - lastRaftPeriodicRun_;
    lastRaftPeriodicRun_ = now;
    
    int msecElapsed = static_cast<int>(delta.dbl() * 1000.0);
    if (msecElapsed <= 0) return;

    static int print_count = 0;
    if (print_count++ < 10) {
        RAFT_LOG("processRaftPeriodic executes! delta=" << delta << ", msecElapsed=" << msecElapsed);
    }

    int old_state = raft_get_state(raftServer_);
    raft_periodic(raftServer_, msecElapsed);
    int new_state = raft_get_state(raftServer_);

    if (old_state != new_state) {
        RAFT_LOG("RAFT STATE CHANGED from " << old_state << " to " << new_state);
        if (new_state == 2 /* RAFT_STATE_CANDIDATE */) {
            RAFT_LOG("I am a candidate now! Starting election...");
        }
    }

    raft_term_t currentTerm = raft_get_current_term(raftServer_);
    if (currentTerm > lastCheckedTerm_) {
        electionRounds_++;
        if (!raft_is_leader(raftServer_)) {
            failedElectionCount_++;
            if (failedElectionCount_ >= maxFailedElections_
                && hasStoppedAtIntersection_
                && !hasCommittedOrder_) {
                isFallbackMode_    = true;
                coordinationMethod_ = "fallback";
                handleFallback();
                return;
            }
        } else {
            failedElectionCount_ = 0;
        }
        lastCheckedTerm_ = currentTerm;
    }

    // Leadership transitions
    bool wasLeader = isLeader_;
    isLeader_      = (raft_is_leader(raftServer_) == 1);

    if (isLeader_ && !wasLeader)       onBecameLeader();
    else if (!isLeader_ && wasLeader)  onLostLeadership();

    // Global timeout fallback — two-phase:
    //
    // Phase 1 (pre-cluster): handled by the 200 ms trigger in checkClusterTrigger;
    //   cluster always forms quickly now, so this is only a safety net.
    //
    // Phase 2 (post-cluster): RAFT must converge within the timeout window.
    //
    // Key fix for large clusters (16+ vehicles):
    //   • For the LEADER: the window starts from ELECTION TIME, not cluster-form time.
    //     A leader elected late (after many merge rounds) gets the full window.
    //   • For FOLLOWERS: if a leader is currently known to RAFT, the global timer is
    //     paused — the leader is working and we must not preempt it.
    if (hasStoppedAtIntersection_ && !hasCommittedOrder_ && !isFallbackMode_) {
        if (timeClusterFormed_ > SIMTIME_ZERO) {
            // For followers with a known leader: skip the timeout, leader is in charge.
            if (!isLeader_ && raftServer_) {
                raft_node_id_t knownLeader = raft_get_current_leader(raftServer_);
                if (knownLeader != -1 /* RAFT_NODE_ID_NONE not guaranteed to be -1, check != 0 */) {
                    // Leader is known — don't time out, wait for its decision.
                    goto skip_timeout;
                }
            }

            // For the leader: window starts from election time so coordination has
            // a full budget regardless of how long cluster formation took.
            simtime_t refTime;
            if (isLeader_ && timeElected_ > SIMTIME_ZERO) {
                refTime = timeElected_;
            } else {
                // Non-leader without a known leader (or leader not yet elected):
                // use the later of stopped-time and cluster-formed-time.
                refTime = (timeStopped_ > timeClusterFormed_) ? timeStopped_ : timeClusterFormed_;
            }

            simtime_t raftWait  = now - refTime;
            // Scale the timeout with cluster size: each extra vehicle adds one
            // extra election-round worth of headroom (worst case: all stagger).
            double extraPerVehicle = electionTimeoutBaseMs_ / 1000.0;
            double raftTimeout = (arrivalWaitTimeMs_ + statusCollectionTimeoutMs_ +
                                  electionTimeoutBaseMs_ * 4 + electionTimeoutJitterMs_ * 2 +
                                  totalVehicles_ * extraPerVehicle * 1000.0 +
                                  3000) / 1000.0;
            if (raftWait.dbl() > raftTimeout) {
                RAFT_LOG("RAFT TIMEOUT: " << raftWait.dbl()*1000.0
                         << "ms since ref (threshold=" << raftTimeout*1000.0 << "ms)");
                handleFallback();
            }
        } else {
            // Pre-cluster: safety net (200 ms trigger should handle this)
            simtime_t discWait    = now - timeStopped_;
            double    discTimeout = 5.0 + totalVehicles_ * 1.0;
            if (discWait.dbl() > discTimeout) {
                RAFT_LOG("DISCOVERY TIMEOUT (safety net): " << discWait.dbl()*1000.0 << "ms");
                handleFallback();
            }
        }
    }
    skip_timeout:;
}

// ============ RAFT STATIC CALLBACKS ============

int RaftAppBase::sendRequestVote(raft_server_t*, void* user_data,
                                 raft_node_t* node, msg_requestvote_t* msg)
{
    return static_cast<RaftAppBase*>(user_data)->doSendRequestVote(node, msg);
}

int RaftAppBase::sendAppendEntries(raft_server_t*, void* user_data,
                                   raft_node_t* node, msg_appendentries_t* msg)
{
    return static_cast<RaftAppBase*>(user_data)->doSendAppendEntries(node, msg);
}

int RaftAppBase::logOffer(raft_server_t*, void* user_data,
                          raft_entry_t* entry, raft_index_t entry_idx)
{
    return static_cast<RaftAppBase*>(user_data)->doLogOffer(entry, entry_idx);
}

int RaftAppBase::applylog(raft_server_t*, void* user_data,
                          raft_entry_t* entry, raft_index_t entry_idx)
{
    return static_cast<RaftAppBase*>(user_data)->doApplyLog(entry, entry_idx);
}

void RaftAppBase::raftLog(raft_server_t*, raft_node_t*, void*, const char*) {}

int RaftAppBase::persistVote(raft_server_t*, void*, raft_node_id_t) { return 0; }

// ============ RAFT LOG CALLBACKS ============

int RaftAppBase::doSendRequestVote(raft_node_t* node, msg_requestvote_t* msg)
{
    RAFT_LOG("sendRequestVote called for node " << raft_node_get_id(node));
    auto data = serializeRequestVote(msg);
    sendRaftUnicast(raft_node_get_id(node) - 1, /*RAFT_REQUEST_VOTE*/ 32, data);
    return 0;
}

int RaftAppBase::doSendAppendEntries(raft_node_t* node, msg_appendentries_t* msg)
{
    if (!node) return -1;
    void* udata = raft_node_get_udata(node);
    int targetVehicleId = static_cast<int>(reinterpret_cast<intptr_t>(udata));
    if (activeVehicles_.find(targetVehicleId) == activeVehicles_.end()) return 0;

    RAFT_LOG_LEADER("SENDING AppendEntries to vehicle " << targetVehicleId
                    << " (n_entries=" << msg->n_entries
                    << ", leader_commit=" << msg->leader_commit << ")");

    auto data = serializeAppendEntries(msg);
    sendRaftUnicast(targetVehicleId, /*RAFT_APPEND_ENTRIES*/ 0x22, data);
    messagesSent_++;
    return 0;
}

int RaftAppBase::doLogOffer(raft_entry_t* entry, raft_index_t entry_idx)
{
    RAFT_LOG("doLogOffer entry #" << entry_idx << " (len=" << entry->data.len << ")");
    return (entry->data.len >= 1) ? 0 : -1;
}

int RaftAppBase::doApplyLog(raft_entry_t* entry, raft_index_t entry_idx)
{
    RAFT_LOG("doApplyLog entry #" << entry_idx
             << " (lastApplied=" << lastAppliedIndex_
             << ", len=" << entry->data.len << ")");

    if (entry_idx <= lastAppliedIndex_) return 0;
    if (entry->data.len < 1)           return 0;

    uint8_t* data       = static_cast<uint8_t*>(entry->data.buf);
    LogEntryType type   = static_cast<LogEntryType>(data[0]);

    // --- STATUS_REPORT ---
    if (type == STATUS_REPORT && entry->data.len >= sizeof(StatusReportEntry) + 1) {
        StatusReportEntry* report = reinterpret_cast<StatusReportEntry*>(data + 1);
        RAFT_LOG("APPLYING STATUS_REPORT entry #" << entry_idx
                 << " (" << report->numVehicles << " vehicles)");

        committedStatuses_.clear();
        for (int i = 0; i < report->numVehicles; i++) {
            committedStatuses_[report->statuses[i].vehicleId] = report->statuses[i];
        }
        logEntriesCommitted_++;
        lastAppliedIndex_ = entry_idx;

        if (isLeader_ && !hasPassedIntersection_ && !hasCommittedOrder_) {
            scheduleOneshotMs(50, [this]() { proposePassOrder(); });
        }
        return 0;
    }

    // --- PASS_ORDER ---
    if (type == PASS_ORDER && entry->data.len >= sizeof(PassScheduleEntry) + 1) {
        PassScheduleEntry* schedule = reinterpret_cast<PassScheduleEntry*>(data + 1);
        RAFT_LOG("APPLYING PASS_ORDER entry #" << entry_idx
                 << " (" << schedule->numBatches << " batches)");

        memcpy(&committedSchedule_, schedule, sizeof(PassScheduleEntry));
        hasCommittedOrder_ = true;
        if (hasStoppedAtIntersection_ && timeStopped_ > SIMTIME_ZERO) {
            timeOrderCommitted_ = NOW;
        }
        logEntriesCommitted_++;
        lastAppliedIndex_ = entry_idx;
        executePassOrder();
        return 0;
    }

    // --- VEHICLE_LEFT ---
    if (type == VEHICLE_LEFT && entry->data.len >= sizeof(VehicleLeftEntry) + 1) {
        VehicleLeftEntry* leftEntry = reinterpret_cast<VehicleLeftEntry*>(data + 1);
        RAFT_LOG("APPLYING VEHICLE_LEFT entry #" << entry_idx
                 << " - vehicle " << leftEntry->vehicleId
                 << " left batch " << leftEntry->batchId);
        logEntriesCommitted_++;
        lastAppliedIndex_ = entry_idx;
        applyVehicleLeftFromRaft(leftEntry->vehicleId, leftEntry->batchId);
        return 0;
    }

    // --- PASS_COMMAND (legacy) ---
    if (type == PASS_COMMAND && entry->data.len >= sizeof(PassCommandEntry) + 1) {
        PassCommandEntry* cmd = reinterpret_cast<PassCommandEntry*>(data + 1);
        RAFT_LOG("APPLYING PASS_COMMAND entry #" << entry_idx
                 << " - vehicle " << cmd->vehicleId);
        logEntriesCommitted_++;
        lastAppliedIndex_ = entry_idx;
        if (cmd->vehicleId == myId_) resumeMovement();
    }

    return 0;
}

// ============ SERIALIZATION ============

std::vector<uint8_t> RaftAppBase::serializeRequestVote(msg_requestvote_t* msg)
{
    std::vector<uint8_t> data(sizeof(*msg));
    memcpy(data.data(), msg, sizeof(*msg));
    return data;
}

std::vector<uint8_t> RaftAppBase::serializeRequestVoteResponse(msg_requestvote_response_t* msg)
{
    std::vector<uint8_t> data(sizeof(*msg));
    memcpy(data.data(), msg, sizeof(*msg));
    return data;
}

std::vector<uint8_t> RaftAppBase::serializeAppendEntries(msg_appendentries_t* msg)
{
    size_t baseSize = offsetof(msg_appendentries_t, entries);
    std::vector<uint8_t> data(baseSize);
    memcpy(data.data(), msg, baseSize);

    if (msg->n_entries > 0 && msg->entries) {
        for (int i = 0; i < msg->n_entries; i++) {
            raft_entry_t* e = &msg->entries[i];
            data.insert(data.end(),
                reinterpret_cast<uint8_t*>(&e->term),
                reinterpret_cast<uint8_t*>(&e->term) + sizeof(e->term));
            data.insert(data.end(),
                reinterpret_cast<uint8_t*>(&e->id),
                reinterpret_cast<uint8_t*>(&e->id) + sizeof(e->id));
            data.insert(data.end(),
                reinterpret_cast<uint8_t*>(&e->type),
                reinterpret_cast<uint8_t*>(&e->type) + sizeof(e->type));
            uint32_t dataLen = e->data.len;
            data.insert(data.end(),
                reinterpret_cast<uint8_t*>(&dataLen),
                reinterpret_cast<uint8_t*>(&dataLen) + sizeof(dataLen));
            if (dataLen > 0 && e->data.buf) {
                uint8_t* buf = static_cast<uint8_t*>(e->data.buf);
                data.insert(data.end(), buf, buf + dataLen);
            }
        }
    }
    return data;
}

std::vector<uint8_t> RaftAppBase::serializeAppendEntriesResponse(msg_appendentries_response_t* msg)
{
    std::vector<uint8_t> data(sizeof(*msg));
    memcpy(data.data(), msg, sizeof(*msg));
    return data;
}

void RaftAppBase::deserializeRequestVote(const std::vector<uint8_t>& data, msg_requestvote_t* msg)
{
    if (data.size() >= sizeof(*msg)) memcpy(msg, data.data(), sizeof(*msg));
}

void RaftAppBase::deserializeRequestVoteResponse(const std::vector<uint8_t>& data, msg_requestvote_response_t* msg)
{
    if (data.size() >= sizeof(*msg)) memcpy(msg, data.data(), sizeof(*msg));
}

void RaftAppBase::deserializeAppendEntries(const std::vector<uint8_t>& data, msg_appendentries_t* msg)
{
    size_t baseSize = offsetof(msg_appendentries_t, entries);
    if (data.size() < baseSize) { msg->entries = nullptr; msg->n_entries = 0; return; }

    memcpy(msg, data.data(), baseSize);

    if (msg->n_entries > 0 && data.size() > baseSize) {
        size_t offset = baseSize;
        msg->entries = new raft_entry_t[msg->n_entries];

        for (int i = 0; i < msg->n_entries && offset < data.size(); i++) {
            raft_entry_t* e = &msg->entries[i];

            if (offset + sizeof(e->term) <= data.size()) {
                memcpy(&e->term, &data[offset], sizeof(e->term)); offset += sizeof(e->term);
            }
            if (offset + sizeof(e->id) <= data.size()) {
                memcpy(&e->id, &data[offset], sizeof(e->id)); offset += sizeof(e->id);
            }
            if (offset + sizeof(e->type) <= data.size()) {
                memcpy(&e->type, &data[offset], sizeof(e->type)); offset += sizeof(e->type);
            }
            uint32_t dataLen = 0;
            if (offset + sizeof(dataLen) <= data.size()) {
                memcpy(&dataLen, &data[offset], sizeof(dataLen)); offset += sizeof(dataLen);
            }
            if (dataLen > 0 && offset + dataLen <= data.size()) {
                e->data.len = dataLen;
                e->data.buf = malloc(dataLen);
                memcpy(e->data.buf, &data[offset], dataLen);
                offset += dataLen;
            } else {
                e->data.len = 0; e->data.buf = nullptr;
            }
        }
    } else {
        msg->entries = nullptr;
    }
}

void RaftAppBase::deserializeAppendEntriesResponse(const std::vector<uint8_t>& data, msg_appendentries_response_t* msg)
{
    if (data.size() >= sizeof(*msg)) memcpy(msg, data.data(), sizeof(*msg));
}

// ============ COORDINATION PROTOCOL ============

void RaftAppBase::sendStatusRequest()
{
    std::cout << simTime() << " [DBG][V" << myId_ << "] SEND_STATUS_REQUEST to "
              << activeVehicles_.size() << " active vehicles, timeout="
              << statusCollectionTimeoutMs_ << "ms" << std::endl;
    RAFT_LOG_LEADER("sending STATUS_REQUEST (timeout=" << statusCollectionTimeoutMs_ << "ms)");

    // Payload carries senderId — must be non-empty for INET UDP compatibility
    std::vector<uint8_t> data(sizeof(int));
    memcpy(data.data(), &myId_, sizeof(int));
    sendRaftBroadcast(/*COORD_STATUS_REQUEST*/ 0x30, data);
    messagesSent_++;

    waitingForStatus_  = true;
    statusResponseCount_ = 0;
    collectedWayOfSight_.clear();
    collectedProposals_.clear();

    // Include our own  wayOfSight
    collectedWayOfSight_[myId_] = wayOfSight_;
    auto myProp = buildMyProposal();
    collectedProposals_[myId_] = myProp;
}

void RaftAppBase::handleStatusRequest(int fromLeader)
{
    if (hasPassedIntersection_) return;
    sendStatusResponse(fromLeader);
}

void RaftAppBase::sendStatusResponse(int toLeader)
{
    VehicleProposal proposal = buildMyProposal();
    std::vector<uint8_t> data(sizeof(VehicleProposal));
    memcpy(data.data(), &proposal, sizeof(VehicleProposal));
    sendRaftUnicast(toLeader, /*COORD_STATUS_RESPONSE*/ 0x31, data);
    messagesSent_++;
}

void RaftAppBase::handleStatusResponse(int fromVehicle, bool wos)
{
    if (!isLeader_ || !waitingForStatus_) return;
    collectedWayOfSight_[fromVehicle] = wos;
    statusResponseCount_++;
    if (statusResponseCount_ >= (int)activeVehicles_.size()) {
        collectStatusAndDecide();
    }
}

void RaftAppBase::handleStatusResponseProposal(int fromVehicle, const VehicleProposal& proposal)
{
    std::cout << simTime() << " [DBG][V" << myId_ << "] STATUS_RESPONSE from V" << fromVehicle
              << " isLeader=" << isLeader_ << " waitingForStatus=" << waitingForStatus_
              << " count=" << statusResponseCount_ << "/" << (totalVehicles_-1)
              << " wos=" << proposal.isFirstInLane
              << " dist=" << proposal.distanceToJunction << "m" << std::endl;
    if (!isLeader_ || !waitingForStatus_) return;
    collectedProposals_[fromVehicle] = proposal;
    collectedWayOfSight_[fromVehicle] = proposal.isFirstInLane;
    statusResponseCount_++;
    std::cout << simTime() << " [DBG][V" << myId_ << "] STATUS collected " << statusResponseCount_
              << "/" << (totalVehicles_-1) << " (need " << (totalVehicles_-1) << ")" << std::endl;
    if (statusResponseCount_ >= totalVehicles_ - 1) {
        waitingForStatus_ = false;
        std::cout << simTime() << " [DBG][V" << myId_ << "] ALL STATUS COLLECTED -> proposePassOrder()" << std::endl;
        proposePassOrder();
    }
}

void RaftAppBase::collectStatusAndDecide()
{
    if (!isLeader_ || hasPassedIntersection_ || hasCommittedOrder_) return;
    waitingForStatus_ = false;

    RAFT_LOG_LEADER("collected status from " << collectedWayOfSight_.size() << " vehicles");
    proposeStatusReport();
}

void RaftAppBase::proposeStatusReport()
{
    if (!raftServer_ || !isLeader_) return;

    RAFT_LOG_LEADER("PROPOSING STATUS_REPORT to quorum");

    StatusReportEntry report;
    memset(&report, 0, sizeof(report));

    for (int vid : activeVehicles_) {
        if (report.numVehicles >= 32) break;
        VehicleStatus& s = report.statuses[report.numVehicles];
        s.vehicleId  = vid;
        s.wayOfSight = collectedWayOfSight_.count(vid) > 0 ? collectedWayOfSight_[vid] : false;

        int vehiclesPerSide = std::max(totalVehicles_ / 4, 1);
        int sideIndex = vid / vehiclesPerSide;
        switch (sideIndex % 4) {
            case 0: strncpy(s.lane, "W2C", 63); break;  // West (swapped with North)
            case 1: strncpy(s.lane, "S2C", 63); break;
            case 2: strncpy(s.lane, "E2C", 63); break;
            case 3: strncpy(s.lane, "N2C", 63); break;  // North (swapped with West)
        }
        s.positionInLane = vid % vehiclesPerSide;
        report.numVehicles++;
    }

    size_t entrySize = 1 + sizeof(StatusReportEntry);
    std::vector<uint8_t> entryBuffer(entrySize);
    entryBuffer[0] = static_cast<uint8_t>(STATUS_REPORT);
    memcpy(entryBuffer.data() + 1, &report, sizeof(StatusReportEntry));

    raft_entry_t entry;
    memset(&entry, 0, sizeof(entry));
    entry.term     = raft_get_current_term(raftServer_);
    entry.type     = RAFT_LOGTYPE_NORMAL;
    entry.data.buf = entryBuffer.data();
    entry.data.len = entrySize;

    msg_entry_response_t response;
    if (raft_recv_entry(raftServer_, &entry, &response) == 0) {
        logEntriesProposed_++;
        RAFT_LOG_LEADER("submitted STATUS_REPORT entry #" << response.idx);

        raft_index_t idx = response.idx;
        scheduleOneshotMs(100, [this, idx]() {
            if (raftServer_ && raft_get_commit_idx(raftServer_) >= idx && !hasCommittedOrder_) {
                if (isLeader_ && !hasPassedIntersection_) proposePassOrder();
            }
        });
    } else {
        RAFT_LOG_LEADER("FAILED to propose STATUS_REPORT");
    }
}

int RaftAppBase::getLaneIndex(const std::string& lane)
{
    if (lane == "N2C") return 3;  // North (swapped with West)
    if (lane == "S2C") return 1;
    if (lane == "E2C") return 2;
    if (lane == "W2C") return 0;  // West (swapped with North)
    return 0;
}

bool RaftAppBase::movementsConflict(int laneA, int turnA, int laneB, int turnB)
{
    if (laneA == laneB) return true;
    bool opposing = (laneA + laneB == 2) || (laneA + laneB == 4);
    if (opposing && turnA == 0 && turnB == 0) return false;
    if (opposing && turnA == 1 && turnB == 1) return false;
    return true;
}

void RaftAppBase::proposePassOrder()
{
    if (!raftServer_ || !isLeader_ || hasCommittedOrder_ || passOrderProposed_) return;
    passOrderProposed_ = true;

    // Leader injects its own proposal (WSM broadcasts don't loop back to sender)
    VehicleProposal myProp = buildMyProposal();
    collectedProposals_[myId_] = myProp;

    // Add default proposals for any active vehicles whose UDP response was lost
    int vehiclesPerSide = std::max(totalVehicles_ / 4, 1);
    for (int vid : activeVehicles_) {
        if (collectedProposals_.count(vid) == 0) {
            VehicleProposal dflt;
            memset(&dflt, 0, sizeof(dflt));
            dflt.vehicleId          = vid;
            dflt.laneIndex          = (vid / vehiclesPerSide) % 4;
            dflt.intendedTurn       = 0;    // assume STRAIGHT
            dflt.isFirstInLane      = true;
            dflt.blockedByVehicleId = -1;
            dflt.waitingTimeMs      = 99999.0; // ensure it's not starved
            dflt.distanceToJunction = 0.0;
            collectedProposals_[vid] = dflt;
            RAFT_LOG_LEADER("vehicle " << vid << " response lost — using default proposal");
        }
    }

    // Sort pool: front vehicles first, then by wait time (fairness), lane, distance
    std::vector<VehicleProposal> pool;
    for (auto& kv : collectedProposals_) pool.push_back(kv.second);

    std::sort(pool.begin(), pool.end(), [](const VehicleProposal& a, const VehicleProposal& b) {
        if (a.isFirstInLane != b.isFirstInLane)  return a.isFirstInLane > b.isFirstInLane;
        double waitDiff = a.waitingTimeMs - b.waitingTimeMs;
        if (std::abs(waitDiff) > 500.0) return waitDiff > 0;
        if (a.laneIndex != b.laneIndex)  return a.laneIndex < b.laneIndex;
        return a.distanceToJunction < b.distanceToJunction;
    });

    PassScheduleEntry schedule;
    memset(&schedule, 0, sizeof(schedule));
    std::set<int> scheduled;

    auto blockerScheduled = [&](const VehicleProposal& v) {
        return (v.blockedByVehicleId < 0) || scheduled.count(v.blockedByVehicleId) > 0;
    };

    while (!pool.empty() && schedule.numBatches < 16) {
        PassBatch& batch = schedule.batches[schedule.numBatches];

        auto primaryIt = pool.begin();
        while (primaryIt != pool.end() && !blockerScheduled(*primaryIt)) ++primaryIt;
        if (primaryIt == pool.end()) primaryIt = pool.begin();

        VehicleProposal primary = *primaryIt;
        batch.vehicleIds[batch.numVehicles++] = primary.vehicleId;
        scheduled.insert(primary.vehicleId);
        pool.erase(primaryIt);

        for (auto it = pool.begin(); it != pool.end() && batch.numVehicles < 8; ) {
            if (!blockerScheduled(*it)) { ++it; continue; }
            bool conflict = false;
            for (int i = 0; i < batch.numVehicles && !conflict; i++) {
                int existingId = batch.vehicleIds[i];
                int lA = collectedProposals_.count(existingId) ?
                         collectedProposals_[existingId].laneIndex : 0;
                int lB = it->laneIndex;
                if (lA == lB) conflict = true;
                else if (!((lA==0&&lB==2)||(lA==2&&lB==0)||(lA==1&&lB==3)||(lA==3&&lB==1)))
                    conflict = true;
            }
            if (!conflict) {
                batch.vehicleIds[batch.numVehicles++] = it->vehicleId;
                scheduled.insert(it->vehicleId);
                it = pool.erase(it);
            } else ++it;
        }
        schedule.numBatches++;
    }

    if (schedule.numBatches == 0) return;

    // Log schedule
    RAFT_LOG_LEADER("PROPOSING PASS_SCHEDULE with " << schedule.numBatches << " batches:");
    for (int b = 0; b < schedule.numBatches; b++) {
        std::cout << "  Batch " << b << ": [";
        for (int v = 0; v < schedule.batches[b].numVehicles; v++) {
            std::cout << schedule.batches[b].vehicleIds[v];
            if (v < schedule.batches[b].numVehicles - 1) std::cout << ",";
        }
        std::cout << "]" << std::endl;
    }

    size_t entrySize = 1 + sizeof(PassScheduleEntry);
    uint8_t* entryData = new uint8_t[entrySize];
    entryData[0] = static_cast<uint8_t>(PASS_ORDER);
    memcpy(entryData + 1, &schedule, sizeof(PassScheduleEntry));

    raft_entry_t entry;
    memset(&entry, 0, sizeof(entry));
    entry.term     = raft_get_current_term(raftServer_);
    entry.type     = RAFT_LOGTYPE_NORMAL;
    entry.data.buf = entryData;
    entry.data.len = entrySize;

    msg_entry_response_t response;
    if (raft_recv_entry(raftServer_, &entry, &response) == 0) {
        logEntriesProposed_++;
        RAFT_LOG_LEADER("submitted PASS_SCHEDULE entry #" << response.idx);

        raft_index_t idx = response.idx;
        scheduleOneshotMs(100, [this, idx, schedule]() {
            if (raftServer_ && raft_get_commit_idx(raftServer_) >= idx && !hasCommittedOrder_) {
                RAFT_LOG_LEADER("APPLYING committed PASS_SCHEDULE #" << idx);
                memcpy(&committedSchedule_, &schedule, sizeof(PassScheduleEntry));
                hasCommittedOrder_  = true;
                timeOrderCommitted_ = NOW;
                logEntriesCommitted_++;
                lastAppliedIndex_   = idx;
                applyCommittedPassOrder();
            }
        });
    } else {
        RAFT_LOG_LEADER("FAILED to propose PASS_SCHEDULE");
        delete[] entryData;
    }
}

void RaftAppBase::executePassOrder()
{
    applyCommittedPassOrder();
}

void RaftAppBase::applyCommittedPassOrder()
{
    if (hasPassedIntersection_) return;

    myBatch_ = -1;
    for (int b = 0; b < committedSchedule_.numBatches; b++) {
        for (int v = 0; v < committedSchedule_.batches[b].numVehicles; v++) {
            if (committedSchedule_.batches[b].vehicleIds[v] == myId_) {
                myBatch_ = b; break;
            }
        }
        if (myBatch_ >= 0) break;
    }

    // Print full committed schedule
    std::cout << simTime() << " [DBG][V" << myId_ << "] PASS_ORDER COMMITTED: numBatches="
              << committedSchedule_.numBatches << " myBatch=" << myBatch_ << std::endl;
    for (int b = 0; b < committedSchedule_.numBatches; b++) {
        std::cout << "  [DBG] Batch " << b << ": [";
        for (int v = 0; v < committedSchedule_.batches[b].numVehicles; v++) {
            std::cout << committedSchedule_.batches[b].vehicleIds[v];
            if (v+1 < committedSchedule_.batches[b].numVehicles) std::cout << ",";
        }
        std::cout << "]" << std::endl;
    }

    if (myBatch_ < 0) {
        RAFT_LOG("NOT SCHEDULED - using fallback");
        std::cout << simTime() << " [DBG][V" << myId_ << "] NOT IN SCHEDULE! Using fallback." << std::endl;
        int fallbackDelayMs = committedSchedule_.numBatches * 2000;
        scheduleOneshotMs(fallbackDelayMs, [this]() {
            if (!hasPassedIntersection_) {
                isFallbackMode_     = true;
                coordinationMethod_ = "fallback";
                resumeMovement();
            }
        });
        return;
    }

    RAFT_LOG("assigned to batch " << myBatch_
             << " (current batch: " << currentBatch_ << ")");

    std::cout << simTime() << " [DBG][V" << myId_ << "] ASSIGNED batch=" << myBatch_
              << " currentBatch=" << currentBatch_
              << " hasCommitted=" << hasCommittedOrder_
              << " hasStopped=" << hasStoppedAtIntersection_ << std::endl;

    if (myBatch_ == 0) {
        std::cout << simTime() << " [DBG][V" << myId_ << "] BATCH 0 -> calling resumeMovement immediately" << std::endl;
        resumeMovement();
    } else {
        // Primary mechanism: checkBatchAdvance() will call resumeMovement() once
        // the previous batch has fully cleared via VEHICLE_LEFT broadcasts.
        // Safety fallback fires if VEHICLE_LEFT messages are lost or ghost vehicles
        // in the schedule never send them. 6 s per batch prevents endless waiting.
        int safetyDelayMs = myBatch_ * 6000;
        std::cout << simTime() << " [DBG][V" << myId_ << "] WAITING for batch " << myBatch_
                  << " (safetyFallback in " << safetyDelayMs << "ms)" << std::endl;
        scheduleOneshotMs(safetyDelayMs, [this, expectedBatch = myBatch_]() {
            std::cout << simTime() << " [DBG][V" << myId_ << "] BATCH SAFETY FALLBACK FIRED for batch "
                      << expectedBatch << " curBatch=" << currentBatch_
                      << " hasPassedIntersection=" << hasPassedIntersection_ << std::endl;
            if (!hasPassedIntersection_ && currentBatch_ < expectedBatch) {
                RAFT_LOG("BATCH SAFETY FALLBACK (lost VEHICLE_LEFT?) for batch " << expectedBatch);
                currentBatch_ = expectedBatch;
                resumeMovement();
            }
        });
        RAFT_LOG("waiting for batch " << myBatch_ << " via checkBatchAdvance");
    }
}

// ============ VEHICLE PASSED / LEFT ============

void RaftAppBase::sendVehiclePassed()
{
    RAFT_LOG("BROADCASTING vehicle-passed");
    std::vector<uint8_t> data = {static_cast<uint8_t>(myId_)};
    sendRaftBroadcast(/*COORD_VEHICLE_PASSED*/ 0x33, data);
    messagesSent_++;
}

void RaftAppBase::handleVehiclePassed(int vehicleId)
{
    std::cout << simTime() << " [DBG][V" << myId_ << "] VEHICLE_PASSED: V" << vehicleId
              << " frontVeh=" << vehicleInFrontOfMe_
              << " myBatch=" << myBatch_ << " curBatch=" << currentBatch_ << std::endl;
    RAFT_LOG("RECEIVED vehicle-passed from vehicle " << vehicleId);
    activeVehicles_.erase(vehicleId);
    if (vehicleId == vehicleInFrontOfMe_) {
        wayOfSight_ = true;
        vehicleInFrontOfMe_ = -1;

        // Chain movement: when the vehicle in front of us passes, we must advance
        // toward the stop line. This applies BOTH to (a) vehicles not yet stopped,
        // AND (b) queued vehicles (hasStoppedAtIntersection_=true) waiting behind.
        // Previously we only did (a) — queued vehicles never advanced when first
        // vehicle left, causing "last vehicle in lane never moves" bug.
        if (!hasPassedIntersection_ && traciVehicle_ && timeStartedMoving_ == SIMTIME_ZERO) {
            std::cout << simTime() << " [CHAIN][V" << myId_ << "] Front V" << vehicleId
                      << " passed -> advancing to stop position (resume car-following)" << std::endl;
            RAFT_LOG("front vehicle passed — advancing to stop position (chain)");
            try {
                traciVehicle_->setSpeedMode(31);  // restore SUMO car-following
                traciVehicle_->setSpeed(-1);      // let SUMO advance us
            } catch (...) {}
        }
    }
}

void RaftAppBase::sendVehicleLeft()
{
    RAFT_LOG("sending vehicle-left (gossip, isLeader=" << isLeader_
             << ", myBatch=" << myBatch_ << ", ttl=" << totalVehicles_ << ")");
    VehicleLeftEntry e; e.vehicleId = myId_; e.batchId = myBatch_;
    std::vector<uint8_t> data(sizeof(VehicleLeftEntry) + 1);
    memcpy(data.data(), &e, sizeof(e));
    data[sizeof(VehicleLeftEntry)] = static_cast<uint8_t>(std::min(totalVehicles_, 255));
    sendRaftBroadcast(/*COORD_VEHICLE_LEFT*/ 0x34, data);
    messagesSent_++;
}

void RaftAppBase::handleVehicleLeft(int vehicleId)
{
    // Legacy path: look up batchId and delegate to gossip handler (ttl=1 = no rebroadcast)
    int batchId = -1;
    for (int b = 0; b < committedSchedule_.numBatches; b++) {
        for (int v = 0; v < committedSchedule_.batches[b].numVehicles; v++) {
            if (committedSchedule_.batches[b].vehicleIds[v] == vehicleId) {
                batchId = b; break;
            }
        }
        if (batchId >= 0) break;
    }
    handleVehicleLeftGossip(vehicleId, batchId, 1);
}

void RaftAppBase::handleVehicleLeftGossip(int vehicleId, int batchId, int ttl)
{
    if (gossipSeenVehicleLeft_.count(vehicleId)) return;
    gossipSeenVehicleLeft_.insert(vehicleId);

    std::cout << simTime() << " [DBG][V" << myId_ << "] VEHICLE_LEFT_GOSSIP: V" << vehicleId
              << " batch=" << batchId << " ttl=" << ttl
              << " curBatch=" << currentBatch_
              << " vehiclesLeftSoFar=[";
    for (int v : vehiclesLeftInBatch_) std::cout << v << " ";
    std::cout << "]" << std::endl;
    RAFT_LOG("RECEIVED vehicle-left (gossip) from vehicle " << vehicleId);

    activeVehicles_.erase(vehicleId);
    vehiclesLeftInBatch_.insert(vehicleId);
    checkBatchAdvance();

    if (ttl > 1) {
        VehicleLeftEntry e; e.vehicleId = vehicleId; e.batchId = batchId;
        std::vector<uint8_t> data(sizeof(VehicleLeftEntry) + 1);
        memcpy(data.data(), &e, sizeof(e));
        data[sizeof(VehicleLeftEntry)] = static_cast<uint8_t>(std::min(ttl - 1, 255));
        sendRaftBroadcast(/*COORD_VEHICLE_LEFT_REBROADCAST*/ 0x35, data);
        messagesSent_++;
    }
}

void RaftAppBase::proposeVehicleLeft(int vehicleId, int batchId)
{
    if (!isLeader_ || !raftServer_ || hasPassedIntersection_) return;

    if (proposedLeft_.count(vehicleId)) return;
    proposedLeft_.insert(vehicleId);

    RAFT_LOG_LEADER("proposing VEHICLE_LEFT for vehicle " << vehicleId
                    << " (batch=" << batchId << ")");

    VehicleLeftEntry leftEntry; leftEntry.vehicleId = vehicleId; leftEntry.batchId = batchId;
    size_t entrySize = 1 + sizeof(VehicleLeftEntry);
    uint8_t* entryData = new uint8_t[entrySize];
    entryData[0] = VEHICLE_LEFT;
    memcpy(entryData + 1, &leftEntry, sizeof(VehicleLeftEntry));

    raft_entry_t entry;
    memset(&entry, 0, sizeof(entry));
    entry.term     = raft_get_current_term(raftServer_);
    entry.type     = RAFT_LOGTYPE_NORMAL;
    entry.data.buf = entryData;
    entry.data.len = entrySize;

    msg_entry_response_t response;
    if (raft_recv_entry(raftServer_, &entry, &response) == 0) {
        logEntriesProposed_++;
    } else {
        delete[] entryData;
    }
}

void RaftAppBase::applyVehicleLeftFromRaft(int vehicleId, int batchId)
{
    RAFT_LOG("RAFT-confirmed: vehicle " << vehicleId
             << " left batch " << batchId);
    activeVehicles_.erase(vehicleId);
    vehiclesLeftInBatch_.insert(vehicleId);
    checkBatchAdvance();
}

void RaftAppBase::checkBatchAdvance()
{
    if (hasPassedIntersection_ || !hasCommittedOrder_) return;
    if (currentBatch_ >= committedSchedule_.numBatches) return;

    PassBatch& batch = committedSchedule_.batches[currentBatch_];
    bool allLeft = true;
    std::string missing = "";
    for (int v = 0; v < batch.numVehicles; v++) {
        int vid = batch.vehicleIds[v];
        if (!vehiclesLeftInBatch_.count(vid)) {
            allLeft = false;
            missing += std::to_string(vid) + " ";
        }
    }

    std::cout << simTime() << " [DBG][V" << myId_ << "] CHECK_BATCH_ADVANCE curBatch=" << currentBatch_
              << " allLeft=" << allLeft
              << " missing=[" << missing << "]"
              << " vehiclesLeft=[";
    for (int v : vehiclesLeftInBatch_) std::cout << v << " ";
    std::cout << "]" << std::endl;

    if (allLeft) {
        currentBatch_++;
        vehiclesLeftInBatch_.clear();
        RAFT_LOG("BATCH ADVANCE: now on batch " << currentBatch_);
        std::cout << simTime() << " [DBG][V" << myId_ << "] BATCH ADVANCED to " << currentBatch_
                  << " myBatch=" << myBatch_ << std::endl;
        if (myBatch_ == currentBatch_ && !hasPassedIntersection_) {
            RAFT_LOG("MY BATCH - starting movement");
            std::cout << simTime() << " [DBG][V" << myId_ << "] MY BATCH NOW ACTIVE -> resumeMovement()" << std::endl;
            resumeMovement();
        }
    }
}

// ============ PROPOSAL BUILDING ============

VehicleProposal RaftAppBase::buildMyProposal()
{
    calculateWayOfSight();  // Bug D: refresh isFirstInLane / vehicleInFrontOfMe_ before proposal

    VehicleProposal p;
    memset(&p, 0, sizeof(p));
    p.vehicleId = myId_;
    strncpy(p.laneEdgeId, myLane_.c_str(), sizeof(p.laneEdgeId) - 1);
    p.laneIndex           = myLaneIndex_;
    p.intendedTurn        = 0;  // STRAIGHT
    p.blockedByVehicleId  = detectBlockingVehicle();
    p.isFirstInLane       = (p.blockedByVehicleId == -1);
    p.waitingTimeMs       = (timeStopped_ > SIMTIME_ZERO)
                            ? (NOW - timeStopped_).dbl() * 1000.0 : 0.0;
    p.distanceToJunction  = calculateDistanceToJunction();

    if (traciVehicle_) {
        try { p.positionOnLane = traciVehicle_->getLanePosition(); p.speed = traciVehicle_->getSpeed(); }
        catch (...) {}
    }
    return p;
}

int RaftAppBase::detectBlockingVehicle()
{
    return vehicleInFrontOfMe_;
}

double RaftAppBase::calculateDistanceToJunction()
{
    return getDistanceToJunction();
}

// ============ INTERSECTION DETECTION ============

void RaftAppBase::checkAndStopAtIntersection()
{
    // Never re-trigger once the vehicle has been given pass permission or has passed.
    if (hasPassedIntersection_ || !traciVehicle_) return;
    if (timeStartedMoving_ > SIMTIME_ZERO) return;  // already cleared to go
    try {
        std::string roadId = traciVehicle_->getRoadId();

        // --- DBG: per-vehicle periodic position report + road transition ---
        bool roadChanged = (roadId != prevRoadId_);
        std::string lastKnownRoad = prevRoadId_;  // save before potential update
        if (roadChanged) {
            prevRoadId_ = roadId;
            double spd = 999.0;
            try { spd = traciVehicle_->getSpeed(); } catch (...) {}
            double d = getDistanceToJunction();
            std::cout << simTime() << " [DBG][V" << myId_ << "] ROAD_CHANGE -> " << roadId
                      << " dist=" << d << "m speed=" << spd << "m/s"
                      << " onApproach=" << (intersectionEdges_.count(roadId) ? "YES" : "NO") << std::endl;
            // Proactively cache approach edge + lane index so cluster-junction detection
            // can use them even if the approach edge was too short to trigger a stop.
            if (intersectionEdges_.count(roadId)) {
                intersectionEdge_ = roadId;
                for (int i = 0; i < (int)approachEdgeList_.size(); i++) {
                    if (approachEdgeList_[i] == roadId) {
                        myLaneIndex_ = i;
                        break;
                    }
                }
            }
        }
        if (NOW - lastStopDebugPrint_ > 5.0) {
            lastStopDebugPrint_ = NOW;
            double spd = 999.0;
            try { spd = traciVehicle_->getSpeed(); } catch (...) {}
            double d = getDistanceToJunction();
            std::cout << simTime() << " [DBG][V" << myId_ << "] POS roadId=" << roadId
                      << " dist=" << d << "m speed=" << spd << "m/s"
                      << " onApproach=" << (intersectionEdges_.count(roadId) ? "YES" : "NO")
                      << " stopped=" << hasStoppedAtIntersection_
                      << " phase=" << clusterPhase_
                      << " leader=" << isLeader_
                      << " committed=" << hasCommittedOrder_
                      << " myBatch=" << myBatch_
                      << " curBatch=" << currentBatch_ << std::endl;
        }

        // Catch vehicles that enter the main intersection cluster junction without being
        // stopped on the approach edge (short approach lanes traversed in <CHECK_INTERVAL).
        bool onClusterJunction = (!roadId.empty() && roadId[0] == ':' &&
                                   roadId.find("cluster") != std::string::npos);
        if (onClusterJunction) {
            if (!hasStoppedAtIntersection_) {
                // If approach edge wasn't proactively cached, try lastKnownRoad as fallback.
                if ((intersectionEdge_.empty() || intersectionEdge_[0] == ':') &&
                    !lastKnownRoad.empty() && lastKnownRoad[0] != ':') {
                    for (int i = 0; i < (int)approachEdgeList_.size(); i++) {
                        if (approachEdgeList_[i] == lastKnownRoad) {
                            intersectionEdge_ = lastKnownRoad;
                            myLaneIndex_ = i;
                            break;
                        }
                    }
                }
                if (intersectionEdge_.empty()) intersectionEdge_ = roadId;
                stopVehicle();
                hasStoppedAtIntersection_ = true;
                timeStopped_ = NOW;
                calculateWayOfSight();
                std::cout << simTime() << " [DBG][V" << myId_ << "] STOPPED (cluster-junction)"
                          << " road=" << roadId
                          << " approachEdge=" << intersectionEdge_
                          << " laneIdx=" << myLaneIndex_
                          << " wos=" << wayOfSight_
                          << " isLeader=" << isLeader_ << std::endl;
                RAFT_LOG("STOPPED (cluster-junction) approachEdge=" << intersectionEdge_);
                if (isLeader_ && !hasCommittedOrder_) {
                    scheduleOneshotMs(arrivalWaitTimeMs_, [this]() {
                        if (isLeader_ && !hasCommittedOrder_ && !hasPassedIntersection_) {
                            sendStatusRequest();
                            scheduleOneshotMs(statusCollectionTimeoutMs_, [this]() {
                                if (isLeader_ && !hasCommittedOrder_ && !hasPassedIntersection_)
                                    collectStatusAndDecide();
                            });
                        }
                    });
                }
            }
            return;  // Do not proceed to approach-edge logic
        }

        if (!intersectionEdges_.count(roadId)) return;

        double dist  = getDistanceToJunction();
        double speed = 999.0;
        try { speed = traciVehicle_->getSpeed(); } catch (...) {}

        // Log raw distance/junction data for stop-position debugging
        double lanePos = 0, laneLen = 0;
        try {
            if (traci_) {
                std::string lid = traciVehicle_->getLaneId();
                lanePos = traciVehicle_->getLanePosition();
                laneLen = traci_->lane(lid).getLength();
            }
        } catch (...) {}
        if (dist >= 0 && dist <= intersectionStopDistance_ * 5.0 && !hasStoppedAtIntersection_) {
            std::cout << simTime() << " [STOP_CHECK][V" << myId_ << "] road=" << roadId
                      << " dist=" << dist << "m (lanePos=" << lanePos << " laneLen=" << laneLen << ")"
                      << " stopDist=" << intersectionStopDistance_ << "m speed=" << speed << "m/s"
                      << std::endl;
        }

        // Front of queue: within stop distance → hard stop and wait for pass order.
        bool closeEnough = (dist >= 0 && dist <= intersectionStopDistance_);
        // Queued behind front: slow/stopped further back.  Only record first detection.
        // Bug 3 fix: speed threshold 0.5→1.5 m/s to better detect queued vehicles
        bool queued = (!closeEnough && speed < 1.5 &&
                       dist >= 0 && dist < intersectionStopDistance_ * 4.0 &&
                       !hasStoppedAtIntersection_);

        if (closeEnough) {
            // Always re-apply the hard stop (vehicle may have advanced in queue to here).
            stopVehicle();
            if (!hasStoppedAtIntersection_) {
                hasStoppedAtIntersection_ = true;
                timeStopped_     = NOW;
                intersectionEdge_ = roadId;
                calculateWayOfSight();
                std::cout << simTime() << " [STOP_CHECK][V" << myId_ << "] STOPPED (front, hard stop) dist="
                          << dist << "m road=" << roadId
                          << " wos=" << wayOfSight_
                          << " frontVeh=" << vehicleInFrontOfMe_
                          << " isLeader=" << isLeader_ << std::endl;
                RAFT_LOG("STOPPED (front) at dist=" << dist << "m");
                if (isLeader_ && !hasCommittedOrder_) {
                    scheduleOneshotMs(arrivalWaitTimeMs_, [this]() {
                        if (isLeader_ && !hasCommittedOrder_ && !hasPassedIntersection_) {
                            sendStatusRequest();
                            scheduleOneshotMs(statusCollectionTimeoutMs_, [this]() {
                                if (isLeader_ && !hasCommittedOrder_ && !hasPassedIntersection_)
                                    collectStatusAndDecide();
                            });
                        }
                    });
                }
            }
        } else if (queued) {
            // Queued vehicle: record for metrics, no hard stop.
            // SUMO car-following holds its queue position naturally.
            // checkAndAdvanceInQueue() will release it via TraCI gap detection.
            hasStoppedAtIntersection_ = true;
            timeStopped_     = NOW;
            intersectionEdge_ = roadId;
            calculateWayOfSight();
            std::cout << simTime() << " [STOP_CHECK][V" << myId_ << "] QUEUED (no hard stop) dist=" << dist
                      << "m speed=" << speed << "m/s road=" << roadId
                      << " wos=" << wayOfSight_
                      << " frontVeh=" << vehicleInFrontOfMe_
                      << " isLeader=" << isLeader_ << std::endl;
            RAFT_LOG("QUEUED at dist=" << dist << "m speed=" << speed << "m/s");
            if (isLeader_ && !hasCommittedOrder_) {
                scheduleOneshotMs(arrivalWaitTimeMs_, [this]() {
                    if (isLeader_ && !hasCommittedOrder_ && !hasPassedIntersection_) {
                        sendStatusRequest();
                        scheduleOneshotMs(statusCollectionTimeoutMs_, [this]() {
                            if (isLeader_ && !hasCommittedOrder_ && !hasPassedIntersection_)
                                collectStatusAndDecide();
                        });
                    }
                });
            }
        }
    } catch (...) {}
}

// TraCI-based queue advancement: on every check-timer tick, ask SUMO who is ahead
// and how far.  When the gap exceeds the stop distance (leader moved away), hand
// control back to SUMO car-following so the whole queue accordions forward.
// Once the vehicle reaches the stop line it is re-caught by checkAndStopAtIntersection.
void RaftAppBase::checkAndAdvanceInQueue()
{
    if (!hasStoppedAtIntersection_ || hasPassedIntersection_ || timeStartedMoving_ > SIMTIME_ZERO) return;
    if (!traciVehicle_) return;
    try {
        double speed = traciVehicle_->getSpeed();
        if (speed > 0.5) return;  // already moving under SUMO

        double dist = getDistanceToJunction();
        if (dist < 0 || dist <= intersectionStopDistance_) return;  // handled by checkAndStop

        // Ask TraCI for the leading vehicle and gap ahead
        auto leader = traciVehicle_->getLeader(intersectionStopDistance_ * 4.0);

        // Gap opened: no leader visible, or gap > 1× stop distance
        bool gapOpened = leader.first.empty() ||
                         leader.second > intersectionStopDistance_;

        if (NOW - lastQueueDebugPrint_ > 1.0) {
            lastQueueDebugPrint_ = NOW;
            std::cout << simTime() << " [QUEUE_ADV][V" << myId_ << "] dist=" << dist
                      << "m speed=" << speed
                      << " leader=" << (leader.first.empty() ? "NONE" : leader.first)
                      << " gap=" << leader.second << "m gapOpened=" << gapOpened
                      << " (need gap>" << intersectionStopDistance_ << "m)"
                      << " myBatch=" << myBatch_ << " curBatch=" << currentBatch_ << std::endl;
        }

        if (gapOpened) {
            std::cout << simTime() << " [QUEUE_ADV][V" << myId_ << "] RELEASING - front moved, taking place"
                      << " dist=" << dist << "m gap=" << leader.second << "m" << std::endl;
            RAFT_LOG("QUEUE ADVANCE (TraCI): gap=" << leader.second
                     << "m dist=" << dist << "m leader=" << leader.first);
            traciVehicle_->setSpeedMode(31);  // all SUMO safety checks on
            traciVehicle_->setSpeed(-1);       // SUMO car-following takes over
        }
    } catch (...) {}
}

bool RaftAppBase::isAtIntersection() const
{
    if (!traciVehicle_) return false;
    try {
        return intersectionEdges_.count(traciVehicle_->getRoadId()) > 0 && isNearJunction();
    } catch (...) { return false; }
}

bool RaftAppBase::isNearJunction() const
{
    double d = getDistanceToJunction();
    return d >= 0 && d <= intersectionStopDistance_;
}

bool RaftAppBase::hasPassedIntersectionEdge() const
{
    if (!traciVehicle_) return false;
    try {
        std::string roadId = traciVehicle_->getRoadId();
        if (exitEdges_.count(roadId)) {
            std::cout << simTime() << " Vehicle " << myId_ << " hasPassed=true (on exit edge " << roadId << ")" << std::endl;
            return true;
        }
        if (timeStartedMoving_ > SIMTIME_ZERO && !roadId.empty()) {
            bool onApproach  = intersectionEdges_.count(roadId) > 0;
            bool onInternal  = (roadId[0] == ':');
            if (!onApproach && !onInternal) {
                std::cout << simTime() << " Vehicle " << myId_ << " hasPassed=true (started moving, left approach/internal. roadId=" << roadId << ")" << std::endl;
                return true;
            }
        }
        return false;
    } catch (...) { return false; }
}

void RaftAppBase::calculateWayOfSight()
{
    // Step 1: determine actual laneIndex from the road this vehicle is currently on.
    // The approach edge list is indexed 0-3 and maps to the 4 intersection directions.
    if (traciVehicle_) {
        try {
            std::string roadId = traciVehicle_->getRoadId();
            // If on a junction internal edge, fall back to the cached approach edge for
            // lane index determination (vehicle may have slipped through a short approach lane).
            const std::string& lookupRoad = (!roadId.empty() && roadId[0] == ':')
                                            ? intersectionEdge_ : roadId;
            for (int i = 0; i < (int)approachEdgeList_.size(); i++) {
                if (approachEdgeList_[i] == lookupRoad) {
                    myLaneIndex_ = i;
                    break;
                }
            }
        } catch (...) {}

        // Step 2: use TraCI leader to determine physical queue position.
        // A vehicle is "first in lane" (wayOfSight=true) when no one is in front.
        try {
            auto leader = traciVehicle_->getLeader(intersectionStopDistance_ * 4.0);
            if (leader.first.empty()) {
                wayOfSight_         = true;
                vehicleInFrontOfMe_ = -1;
            } else {
                wayOfSight_         = false;
                vehicleInFrontOfMe_ = -1;  // SUMO ID can't be directly mapped to OMNeT++ index
            }
            std::cout << simTime() << " [DBG][V" << myId_ << "] WOS_CALC"
                      << " laneIdx=" << myLaneIndex_
                      << " wos=" << wayOfSight_
                      << " traciLeader=" << (leader.first.empty() ? "NONE" : leader.first)
                      << " gap=" << leader.second << std::endl;
        } catch (...) {
            wayOfSight_         = true;
            vehicleInFrontOfMe_ = -1;
        }
        return;
    }

    // Fallback: no TraCI yet — use module-index formula (less accurate).
    int vehiclesPerSide = std::max(totalVehicles_ / 4, 1);
    int positionInLane  = myId_ % vehiclesPerSide;
    if (positionInLane == 0) {
        wayOfSight_         = true;
        vehicleInFrontOfMe_ = -1;
    } else {
        wayOfSight_         = true;
        vehicleInFrontOfMe_ = -1;
    }
}

void RaftAppBase::checkIfLeftIntersection()
{
    if (!hasPassedIntersection_ && hasPassedIntersectionEdge()) {
        hasPassedIntersection_ = true;
        timePassed_            = NOW;
        RAFT_LOG("LEFT intersection");
        sendVehiclePassed();
        sendVehicleLeft();
        outputMetricsJSON();
    }
}

void RaftAppBase::stopVehicle()
{
    if (!traciVehicle_) return;
    try { 
        traciVehicle_->setSpeedMode(0);
        traciVehicle_->setSpeed(0); 
    }
    catch (...) { RAFT_LOG_ERR("could not stop vehicle"); }
}

void RaftAppBase::resumeMovement()
{
    if (!traciVehicle_ || hasPassedIntersection_) return;
    std::cout << simTime() << " [DBG][V" << myId_ << "] RESUME_MOVEMENT called"
              << " hasStopped=" << hasStoppedAtIntersection_
              << " myBatch=" << myBatch_
              << " curBatch=" << currentBatch_
              << " isLeader=" << isLeader_
              << " fallback=" << isFallbackMode_ << std::endl;
    timeStartedMoving_ = NOW;
    RAFT_LOG("RESUMING movement");
    try {
        traciVehicle_->setSpeedMode(0);
        traciVehicle_->setParameter("jmIgnoreFoeProb",  "1.0");
        traciVehicle_->setParameter("jmIgnoreFoeSpeed", "100.0");
        traciVehicle_->setParameter("jmTimegapMinor",   "0.0");
        double speed = traciVehicle_->getMaxSpeed();
        if (speed <= 0) speed = 13.89;
        traciVehicle_->setSpeed(speed);
        timeStartedMoving_ = NOW;
    } catch (...) { RAFT_LOG_ERR("could not resume speed"); }
}

void RaftAppBase::onBecameLeader()
{
    std::cout << simTime() << " [DBG][V" << myId_ << "] BECAME LEADER term="
              << raft_get_current_term(raftServer_)
              << " stopped=" << hasStoppedAtIntersection_
              << " committed=" << hasCommittedOrder_
              << " activeVehicles=[";
    for (int v : activeVehicles_) std::cout << v << " ";
    std::cout << "]" << std::endl;
    RAFT_LOG("BECAME LEADER (term=" << raft_get_current_term(raftServer_) << ")");
    wasElectedLeader_     = true;
    timeElected_          = NOW;
    failedElectionCount_  = 0;
    if (hasStoppedAtIntersection_ && !hasCommittedOrder_) {
        // Broadcast status request so followers send their VehicleProposal
        sendStatusRequest();
        // After timeout, force collection with whatever proposals arrived
        scheduleOneshotMs(statusCollectionTimeoutMs_, [this]() {
            if (isLeader_ && !hasCommittedOrder_ && !hasPassedIntersection_)
                collectStatusAndDecide();
        });
    }
}

void RaftAppBase::onLostLeadership()
{
    RAFT_LOG("LOST leadership");
    waitingForStatus_ = false;
}

void RaftAppBase::handleFallback()
{
    if (hasPassedIntersection_) return;
    isFallbackMode_     = true;
    coordinationMethod_ = "fallback";
    timeOrderCommitted_ = NOW;
    std::cout << simTime() << " [DBG][V" << myId_ << "] FALLBACK ACTIVATED"
              << " wos=" << wayOfSight_
              << " frontVeh=" << vehicleInFrontOfMe_
              << " failedElections=" << failedElectionCount_
              << " stopped=" << hasStoppedAtIntersection_
              << " committed=" << hasCommittedOrder_
              << " isLeader=" << isLeader_ << std::endl;
    RAFT_LOG("FALLBACK MODE activated");
    if (wayOfSight_ || vehicleInFrontOfMe_ == -1
        || !activeVehicles_.count(vehicleInFrontOfMe_)) {
        resumeMovement();
    } else {
        // Vehicle is queued behind another. Estimate queue position from lane layout
        // (vehiclesPerSide vehicles per approach) and stagger departure to avoid
        // simultaneous movement into the intersection.
        // Also handles the all-fallback deadlock: when everyone falls back at once,
        // activeVehicles_ still contains all vehicles so nobody would resume without
        // this timer. Staggered by 2 s per position so only the front vehicle goes first.
        int vehiclesPerSide = std::max(totalVehicles_ / 4, 1);
        int posInLane       = myId_ % vehiclesPerSide;
        int delayMs         = fallbackWaitMinMs_ + posInLane * 2000;
        RAFT_LOG("FALLBACK queued (pos=" << posInLane << ") — forced resume in " << delayMs << "ms");
        scheduleOneshotMs(delayMs, [this]() {
            if (!hasPassedIntersection_) resumeMovement();
        });
    }
}

// ============ METRICS OUTPUT ============

void RaftAppBase::outputMetricsJSON()
{
    if (metricsWritten_) return;
    metricsWritten_ = true;

    RaftMetrics::writeVehicleJSON(
        myId_,
        myLane_,
        myRoute_,
        wasElectedLeader_,
        coordinationMethod_,
        transportName_,
        timeStopped_.dbl()       * 1000.0,
        timeClusterFormed_.dbl() * 1000.0,
        timeElected_.dbl()       * 1000.0,
        timeOrderCommitted_.dbl()* 1000.0,
        timeStartedMoving_.dbl() * 1000.0,
        timePassed_.dbl()        * 1000.0,
        messagesSent_,
        messagesReceived_,
        electionRounds_,
        logEntriesProposed_,
        logEntriesCommitted_,
        myBatch_
    );
}
