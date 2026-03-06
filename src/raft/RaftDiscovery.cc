// RaftDiscovery.cc — Membership discovery, cluster invitation, and cluster formation/merging

#include "raft/RaftAppBase.h"
#include "raft/RaftLogger.h"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <set>

#define NOW (simTime())

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

// Payload: [vehicleId:4B][clusterId:4B][timestamp:8B][numMembers:1B][member0:4B]...
static constexpr int MSG_CLUSTER_INVITATION = 0x13;

void RaftAppBase::sendClusterInvitation()
{
    if (hasPassedIntersection_) return;

    // Initialize cluster identity on first send
    if (myClusterTimestamp_ < 0) {
        myClusterId_        = myId_;
        myClusterTimestamp_ = NOW.dbl();
    }

    // Members: for DISCOVERY use self only; for COORDINATION use activeVehicles_
    std::set<int> members;
    if (clusterPhase_ == PHASE_DISCOVERY) {
        members.insert(myId_);
    } else {
        members = activeVehicles_;
        members.insert(myId_);
    }

    size_t payloadSize = sizeof(int) * 2 + sizeof(double) + 1 + members.size() * sizeof(int);
    if (members.size() > 255) payloadSize = 0;
    std::vector<uint8_t> data(payloadSize);
    if (payloadSize == 0) return;

    size_t off = 0;
    memcpy(data.data() + off, &myId_, sizeof(int)); off += sizeof(int);
    memcpy(data.data() + off, &myClusterId_, sizeof(int)); off += sizeof(int);
    memcpy(data.data() + off, &myClusterTimestamp_, sizeof(double)); off += sizeof(double);
    data[off++] = static_cast<uint8_t>(std::min(members.size(), (size_t)255));
    for (int v : members) {
        if (off + sizeof(int) <= data.size()) {
            memcpy(data.data() + off, &v, sizeof(int));
            off += sizeof(int);
        }
    }
    sendRaftBroadcast(MSG_CLUSTER_INVITATION, data);
    messagesSent_++;
}

void RaftAppBase::handleClusterInvitation(const std::vector<uint8_t>& data, int senderId)
{
    if (senderId == myId_) return;
    if (senderId < 0 || senderId >= totalVehicles_) return;
    if (data.size() < sizeof(int) * 2 + sizeof(double) + 1) return;

    int theirVehicleId, theirClusterId;
    double theirTimestamp;
    uint8_t numMembers;
    size_t off = 0;
    memcpy(&theirVehicleId,  data.data() + off, sizeof(int)); off += sizeof(int);
    memcpy(&theirClusterId,  data.data() + off, sizeof(int)); off += sizeof(int);
    memcpy(&theirTimestamp,  data.data() + off, sizeof(double)); off += sizeof(double);
    numMembers = data[off++];
    if (data.size() < off + numMembers * sizeof(int)) return;

    std::set<int> theirMembers;
    for (int i = 0; i < numMembers; i++) {
        int vid;
        memcpy(&vid, data.data() + off + i * sizeof(int), sizeof(int));
        if (vid >= 0 && vid < totalVehicles_) theirMembers.insert(vid);
    }

    // DISCOVERY phase: absorb peers, update our cluster identity if theirs wins
    if (clusterPhase_ == PHASE_DISCOVERY) {
        for (int v : theirMembers) {
            if (v != myId_) discoveredPeers_.insert(v);
        }
        activeVehicles_.insert(theirMembers.begin(), theirMembers.end());

        // Adopt their (clusterId, timestamp) if incoming wins (older ts, or tie-break lower id)
        if (myClusterTimestamp_ < 0) {
            myClusterId_        = myId_;
            myClusterTimestamp_ = NOW.dbl();
        }
        bool theirWins = (theirTimestamp < myClusterTimestamp_) ||
                        (theirTimestamp == myClusterTimestamp_ && theirClusterId < myClusterId_);
        if (theirWins) {
            myClusterId_        = theirClusterId;
            myClusterTimestamp_  = theirTimestamp;
            for (int v : theirMembers) discoveredPeers_.insert(v);
        }
        return;
    }

    // PHASE_COORDINATION: run merge logic (same as handleClusterExists with timestamp+clusterId)
    if (clusterPhase_ != PHASE_COORDINATION) return;

    std::set<int> currentMembers = activeVehicles_;
    currentMembers.insert(myId_);
    std::set<int> mergedMembers = currentMembers;
    mergedMembers.insert(theirMembers.begin(), theirMembers.end());
    bool unionAddsMembers = (mergedMembers.size() > currentMembers.size());

    bool canMerge = !hasCommittedOrder_ && !hasPassedIntersection_;
    if (canMerge && lastMergeTime_ > SIMTIME_ZERO) {
        double cooldownSec = mergeCooldownMs_ / 1000.0;
        if ((NOW - lastMergeTime_).dbl() < cooldownSec)
            canMerge = false;
    }

    // Timestamp+clusterId merge rule: incoming wins if (older ts) or (same ts, lower clusterId)
    bool incomingWins = (theirTimestamp < myClusterTimestamp_) ||
                        (theirTimestamp == myClusterTimestamp_ && theirClusterId < myClusterId_);

    if (canMerge && unionAddsMembers && incomingWins) {
        std::cout << simTime() << " [DBG][V" << myId_
                  << "] MERGE (invitation): theirTs=" << theirTimestamp
                  << " theirId=" << theirClusterId
                  << " -> merging into " << mergedMembers.size() << " members" << std::endl;
        myClusterId_       = theirClusterId;
        myClusterTimestamp_ = theirTimestamp;
        mergeIntoLargerCluster(mergedMembers);
        return;
    }

    if (hasCommittedOrder_ && unionAddsMembers) {
        for (int v : theirMembers) {
            if (v != myId_ && currentMembers.count(v) == 0) {
                // Add random jitter based on our ID to prevent simultaneous LATE_JOIN_ORDER broadcast storms
                double jitterMs = getRandomDouble(10.0, 150.0);
                scheduleOneshotMs(jitterMs, [this, v]() {
                    sendLateJoinOrderTo(v);
                });
            }
        }
    }
    for (int v : theirMembers) activeVehicles_.insert(v);
}

// ============ RAFT SINGLE-NODE INIT (early start) ============
// Each vehicle starts RAFT at init with self only. Merges form larger cluster via invitation.

void RaftAppBase::initRaftSingleNode()
{
    if (raftServer_) return;  // Already initialized

    raft_server_t* s = raft_new();
    if (!s) {
        RAFT_LOG_ERR("raft_new() returned nullptr!");
        return;
    }
    raftServer_ = s;

    raft_cbs_t cbs;
    memset(&cbs, 0, sizeof(cbs));
    cbs.send_requestvote   = &RaftAppBase::sendRequestVote;
    cbs.send_appendentries  = &RaftAppBase::sendAppendEntries;
    cbs.log_offer           = &RaftAppBase::logOffer;
    cbs.applylog            = &RaftAppBase::applylog;
    cbs.log                 = &RaftAppBase::raftLog;
    cbs.persist_vote        = &RaftAppBase::persistVote;
    cbs.log_get_node_id     = &RaftAppBase::logGetNodeId;
    raft_set_callbacks(raftServer_, &cbs, this);

    int electionTimeout = electionTimeoutBaseMs_ + (int)getRandomDouble(0.0, (double)electionTimeoutJitterMs_);
    raft_set_election_timeout(raftServer_, electionTimeout);
    raft_set_request_timeout(raftServer_, requestTimeoutMs_);

    myRaftNodeId_ = getNodeIdFromVehicleId(myId_);
    void* udata = reinterpret_cast<void*>(static_cast<intptr_t>(myId_));
    raft_add_node(raftServer_, udata, myRaftNodeId_, 1);

    clusterPhase_   = PHASE_COORDINATION;
    clusterFormed_  = true;
    timeClusterFormed_ = NOW;
    activeVehicles_.clear();
    activeVehicles_.insert(myId_);

    if (myClusterTimestamp_ < 0) {
        myClusterId_ = myId_;
        myClusterTimestamp_ = NOW.dbl();
    }

    RAFT_LOG("RAFT started single-node V" << myId_);
    onClusterFormed();
}

// ============ CLUSTER FORMATION (used by merge) ============

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

    /* Mark inactive any vehicles that left before we formed (late-joiner case).
     * Quorum = majority of remaining; prevents waiting for ACKs from departed vehicles. */
    for (int vid : vehiclesLeftBeforeFormed_) {
        if (vid == myId_) continue;
        raft_node_t* node = raft_get_node(raftServer_, getNodeIdFromVehicleId(vid));
        if (node) {
            raft_node_set_active(node, 0);
            RAFT_LOG("RAFT node for vehicle " << vid << " marked INACTIVE (left before we formed)");
        }
    }
    vehiclesLeftBeforeFormed_.clear();

    clusterPhase_ = PHASE_COORDINATION;

    // Call virtual hook for transport-specific logic
    onClusterFormed();

    // Broadcast cluster formation to peers
    broadcastClusterForm();

    // Keep broadcasting cluster membership until vehicle leaves intersection.
    // Never stop for hasCommittedOrder_ — enables late merges and late joiners.
    for (int delayMs = 300; delayMs <= 60000; delayMs += 2000) {
        scheduleOneshotMs(delayMs, [this]() {
            if (clusterPhase_ == PHASE_COORDINATION && !hasPassedIntersection_)
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

    // 2. Reset all RAFT-specific state (including decision metrics from sub-cluster).
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
    proposedTimes_.clear();
    totalRaftDecisionTimeSec_ = 0.0;
    statusCollectionTimeMs_   = 0.0;
    timeStatusRequestSent_    = SIMTIME_ZERO;
    timeElected_              = SIMTIME_ZERO;

    // 3. Absorb all merged members into our discovery set.
    for (int v : mergedMembers)
        if (v != myId_) discoveredPeers_.insert(v);

    // 4. Reset phase flags so formCluster() accepts the call.
    clusterPhase_   = PHASE_DISCOVERY;
    clusterFormed_  = false;

    // 5. Record merge time for cooldown.
    lastMergeTime_ = NOW;

    // 6. Re-form the cluster with the larger unified member set.
    formCluster(mergedMembers);
}

// ============ CLUSTER BROADCAST ============

void RaftAppBase::broadcastClusterForm()
{
    // Payload: [clusterId(4B)][timestamp(8B)][numMembers(1B)][member0(4B)]...
    if (myClusterTimestamp_ < 0) {
        myClusterId_        = myId_;
        myClusterTimestamp_ = NOW.dbl();
    }
    std::vector<uint8_t> data(sizeof(int) + sizeof(double) + 1 + activeVehicles_.size() * sizeof(int));
    size_t off = 0;
    memcpy(data.data() + off, &myClusterId_, sizeof(int)); off += sizeof(int);
    memcpy(data.data() + off, &myClusterTimestamp_, sizeof(double)); off += sizeof(double);
    data[off++] = static_cast<uint8_t>(activeVehicles_.size());
    for (int vid : activeVehicles_) {
        memcpy(data.data() + off, &vid, sizeof(int)); off += sizeof(int);
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
        handleClusterExists(data, senderId);
        return;
    }
    // New format: [clusterId(4B)][timestamp(8B)][numMembers(1B)][member0(4B)]...
    // Legacy format: [numMembers(1B)][member0(4B)]...
    if (data.size() < 1) return;
    size_t off = 0;
    int theirClusterId = 0;
    double theirTimestamp = 0;
    uint8_t n;
    if (data.size() >= sizeof(int) + sizeof(double) + 1) {
        memcpy(&theirClusterId, data.data(), sizeof(int)); off += sizeof(int);
        memcpy(&theirTimestamp, data.data() + off, sizeof(double)); off += sizeof(double);
    }
    n = data[off++];
    if (data.size() < off + n * sizeof(int)) return;

    for (int i = 0; i < n; i++) {
        int vid;
        memcpy(&vid, data.data() + off + i * sizeof(int), sizeof(int));
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
    // Adopt (clusterId, timestamp) if we have them and incoming wins
    if (data.size() >= sizeof(int) + sizeof(double) + 1) {
        if (myClusterTimestamp_ < 0) {
            myClusterId_        = myId_;
            myClusterTimestamp_ = NOW.dbl();
        }
        bool theirWins = (theirTimestamp < myClusterTimestamp_) ||
                        (theirTimestamp == myClusterTimestamp_ && theirClusterId < myClusterId_);
        if (theirWins) {
            myClusterId_       = theirClusterId;
            myClusterTimestamp_ = theirTimestamp;
        }
    }
}

void RaftAppBase::handleClusterExists(const std::vector<uint8_t>& data, int senderId)
{
    if (data.size() < 1) return;
    size_t off = 0;
    int theirClusterId = 0;
    double theirTimestamp = 0;
    uint8_t n;
    if (data.size() >= sizeof(int) + sizeof(double) + 1) {
        memcpy(&theirClusterId, data.data(), sizeof(int)); off += sizeof(int);
        memcpy(&theirTimestamp, data.data() + sizeof(int), sizeof(double)); off += sizeof(double);
    }
    n = data[off++];
    if (data.size() < off + n * sizeof(int)) return;

    std::set<int> incomingMembers;
    for (int i = 0; i < n; i++) {
        int vid;
        memcpy(&vid, data.data() + off + i * sizeof(int), sizeof(int));
        if (vid >= 0 && vid < totalVehicles_)
            incomingMembers.insert(vid);
    }

    if (clusterPhase_ == PHASE_DISCOVERY) {
        for (int v : incomingMembers) {
            if (v != myId_) discoveredPeers_.insert(v);
        }
        if (data.size() >= sizeof(int) + sizeof(double) + 1) {
            if (myClusterTimestamp_ < 0) {
                myClusterId_        = myId_;
                myClusterTimestamp_ = NOW.dbl();
            }
            bool theirWins = (theirTimestamp < myClusterTimestamp_) ||
                            (theirTimestamp == myClusterTimestamp_ && theirClusterId < myClusterId_);
            if (theirWins) {
                myClusterId_       = theirClusterId;
                myClusterTimestamp_ = theirTimestamp;
            }
        }
        return;
    }

    // ---- PHASE_COORDINATION ----
    std::set<int> currentMembers = activeVehicles_;
    currentMembers.insert(myId_);
    std::set<int> mergedMembers = currentMembers;
    mergedMembers.insert(incomingMembers.begin(), incomingMembers.end());
    bool unionAddsMembers = (mergedMembers.size() > currentMembers.size());

    // Timestamp+clusterId merge rule: incoming wins if (older ts) or (same ts, lower clusterId)
    // Legacy format (no timestamp): fall back to size-based (incoming bigger or equal-size disjoint)
    bool incomingWins;
    if (data.size() >= sizeof(int) + sizeof(double) + 1) {
        incomingWins = (theirTimestamp < myClusterTimestamp_) ||
                      (theirTimestamp == myClusterTimestamp_ && theirClusterId < myClusterId_);
    } else {
        bool incomingBigger  = (incomingMembers.size() > currentMembers.size());
        bool equalSizeMerge = (incomingMembers.size() == currentMembers.size() && unionAddsMembers);
        incomingWins = incomingBigger || equalSizeMerge;
    }

    bool canMerge = !hasCommittedOrder_ && !hasPassedIntersection_;
    if (canMerge && lastMergeTime_ > SIMTIME_ZERO) {
        double cooldownSec = mergeCooldownMs_ / 1000.0;
        if ((NOW - lastMergeTime_).dbl() < cooldownSec)
            canMerge = false;
    }

    if (canMerge && unionAddsMembers && incomingWins) {
        std::cout << simTime() << " [DBG][V" << myId_
                  << "] MERGE: incoming=" << incomingMembers.size()
                  << " mine=" << currentMembers.size()
                  << " -> merging into " << mergedMembers.size() << " members" << std::endl;
        if (data.size() >= sizeof(int) + sizeof(double) + 1) {
            myClusterId_       = theirClusterId;
            myClusterTimestamp_ = theirTimestamp;
        }
        mergeIntoLargerCluster(mergedMembers);
        return;
    }

    // Phase 2: Post-commit late joiner — if we've committed and there are new vehicles,
    // send them the schedule so they can join without participating in RAFT.
    if (hasCommittedOrder_ && unionAddsMembers) {
        for (int v : incomingMembers) {
            if (v != myId_ && currentMembers.count(v) == 0) {
                double jitterMs = getRandomDouble(10.0, 150.0);
                scheduleOneshotMs(jitterMs, [this, v]() {
                    sendLateJoinOrderTo(v);
                });
            }
        }
    }

    // No merge — just update the active-vehicle set with any new arrivals.
    for (int v : incomingMembers)
        activeVehicles_.insert(v);
}

// ============ LATE JOIN ORDER ============

void RaftAppBase::sendLateJoinOrderTo(int targetVehicleId)
{
    if (!hasCommittedOrder_ || hasPassedIntersection_) return;

    // Build extended schedule: add new batch containing only the late joiner
    PassScheduleEntry extendedSchedule;
    memset(&extendedSchedule, 0, sizeof(extendedSchedule));
    extendedSchedule.numBatches = committedSchedule_.numBatches + 1;
    for (int b = 0; b < committedSchedule_.numBatches; b++)
        extendedSchedule.batches[b] = committedSchedule_.batches[b];
    extendedSchedule.batches[committedSchedule_.numBatches].numVehicles = 1;
    extendedSchedule.batches[committedSchedule_.numBatches].vehicleIds[0] = targetVehicleId;

    int assignBatch = committedSchedule_.numBatches;  // new batch index
    double consensusTimeSec = timeOrderCommitted_.dbl();  // when main cluster committed (for metrics)
    size_t payloadSize = sizeof(PassScheduleEntry) + sizeof(int) * 2 + sizeof(double);
    std::vector<uint8_t> data(payloadSize);
    memcpy(data.data(), &extendedSchedule, sizeof(PassScheduleEntry));
    memcpy(data.data() + sizeof(PassScheduleEntry), &assignBatch, sizeof(int));
    memcpy(data.data() + sizeof(PassScheduleEntry) + sizeof(int), &currentBatch_, sizeof(int));
    memcpy(data.data() + sizeof(PassScheduleEntry) + sizeof(int) * 2, &consensusTimeSec, sizeof(double));

    sendRaftToPeer(targetVehicleId, /*LATE_JOIN_ORDER*/ 0x40, data);
    messagesSent_++;
    std::cout << simTime() << " [DBG][V" << myId_ << "] LATE_JOIN_ORDER sent to V"
              << targetVehicleId << " assignBatch=" << assignBatch
              << " currentBatch=" << currentBatch_ << std::endl;
}

void RaftAppBase::handleLateJoinOrder(const std::vector<uint8_t>& data, int senderId)
{
    if (hasPassedIntersection_) return;
    size_t required = sizeof(PassScheduleEntry) + sizeof(int) * 2 + sizeof(double);
    if (data.size() < required) {
        // Backward compat: old payload without consensus time
        required = sizeof(PassScheduleEntry) + sizeof(int) * 2;
        if (data.size() < required) return;
    }

    // Idempotent: if we already have a committed order (from raft or previous late-join), ignore
    if (hasCommittedOrder_) return;

    std::cout << simTime() << " [DBG][V" << myId_ << "] LATE_JOIN_ORDER received from V"
              << senderId << " — joining committed cluster" << std::endl;

    // Free our RAFT instance if we had one (ghost cluster)
    if (raftServer_) {
        raft_free(raftServer_);
        raftServer_ = nullptr;
    }
    isLeader_ = false;

    // Parse payload
    memcpy(&committedSchedule_, data.data(), sizeof(PassScheduleEntry));
    int assignBatch, senderCurrentBatch;
    memcpy(&assignBatch, data.data() + sizeof(PassScheduleEntry), sizeof(int));
    memcpy(&senderCurrentBatch, data.data() + sizeof(PassScheduleEntry) + sizeof(int), sizeof(int));
    if (data.size() >= sizeof(PassScheduleEntry) + sizeof(int) * 2 + sizeof(double)) {
        double consensusTimeSec;
        memcpy(&consensusTimeSec, data.data() + sizeof(PassScheduleEntry) + sizeof(int) * 2, sizeof(double));
        timeOrderCommitted_ = SimTime(consensusTimeSec, SIMTIME_S);  // use real consensus time for metrics
    } else {
        timeOrderCommitted_ = NOW;
    }

    myBatch_           = assignBatch;
    currentBatch_      = senderCurrentBatch;
    hasCommittedOrder_ = true;
    clusterPhase_      = PHASE_COORDINATION;
    clusterFormed_     = true;
    timeClusterFormed_ = timeClusterFormed_ > SIMTIME_ZERO ? timeClusterFormed_ : NOW;
    coordinationMethod_ = "raft";  // got decision via raft cluster (late join path)

    // Rebuild activeVehicles_ from schedule + self
    activeVehicles_.clear();
    for (int b = 0; b < committedSchedule_.numBatches; b++) {
        for (int v = 0; v < committedSchedule_.batches[b].numVehicles; v++)
            activeVehicles_.insert(committedSchedule_.batches[b].vehicleIds[v]);
    }
    activeVehicles_.insert(myId_);

    if (myBatch_ == 0 || myBatch_ == currentBatch_) {
        resumeMovement();
    } else {
        int safetyDelayMs = myBatch_ * 6000;
        scheduleOneshotMs(safetyDelayMs, [this, expectedBatch = myBatch_]() {
            if (!hasPassedIntersection_ && currentBatch_ < expectedBatch) {
                currentBatch_ = expectedBatch;
                resumeMovement();
            }
        });
    }
    // Schedule 1.5s timeout for current batch (VEHICLE_LEFT may be lost)
    if (currentBatch_ < committedSchedule_.numBatches) {
        scheduleVehicleLeftTimeout(currentBatch_);
    }
}
