// RaftDiscovery.cc — Pre-intersection discovery, lane-leader election,
//                    and RAFT cluster formation.
//
// Protocol phases:
//   1. DISCOVERY: All vehicles broadcast PEER_BEACON (own VehicleProposal).
//      All vehicles build vehicleDB_[senderId] = proposal.
//      isLaneLeader_ flag is recomputed every check interval.
//
//   2. INTERSECTION: When stopped, each lane leader computes the cluster members
//      directly from its own vehicleDB_ (front vehicle of each approach lane),
//      then sends CLUSTER_JOIN_INVITE to all of them.  No pre-election DB exchange.
//      The leader keeps retrying every 500 ms until raftStarted_ is set.
//
//   3. RAFT: formCluster() initialises the raft_server_t with the agreed member
//      set and triggers election. Everything after this is handled by RaftCore.cc.
//      After election, the RAFT leader collects each lane leader's full vehicleDB_
//      via STATUS_REQUEST / COORD_DB_RESPONSE and uses that for pass-order decisions.

#include "raft/RaftAppBase.h"
#include "raft/RaftLogger.h"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <set>

#define NOW (simTime())

// ============ PRE-INTERSECTION PEER BEACON ============

// Broadcast own VehicleProposal + cert so peers can verify isPriority and build vehicleDB_.
void RaftAppBase::sendPeerBeacon()
{
    if (hasPassedIntersection_) return;

    VehicleProposal myProposal = updateMyProposal();
    myProposal.isPriority = false;  // receiver sets this after cert verification, not self-claim
    vehicleDB_[myId_] = myProposal;
    vehicleDB_[myId_].isPriority = isPriorityVehicle_;  // self knows own role

    // Pack SignedProposal so receivers can authenticate isPriority
    SignedProposal sp;
    memset(&sp, 0, sizeof(sp));
    sp.proposalSize = sizeof(VehicleProposal);
    memcpy(sp.proposalBytes, &myProposal, sizeof(VehicleProposal));
    sp.timestampMs  = (uint64_t)(simTime().dbl() * 1000.0);
    sp.cert         = myCert_;
    CryptoAuth::instance().signProposal(myPrivKey_,
                                         sp.proposalBytes, sp.proposalSize,
                                         sp.timestampMs, sp.signature, sp.signatureLen);

    std::vector<uint8_t> data(sizeof(SignedProposal));
    memcpy(data.data(), &sp, sizeof(SignedProposal));
    // Send proposal to everyone
    sendRaftBroadcast(/*PEER_BEACON*/ 0x10, data);
    messagesSent_++;
}

// Receive a PEER_BEACON: verify cert, set isPriority in vehicleDB_.
void RaftAppBase::handlePeerBeacon(const std::vector<uint8_t>& data, int senderId)
{
    if (senderId == myId_) return;
    if (senderId < 0 || senderId >= totalVehicles_) return;

    VehicleProposal proposal;
    bool isPrio = false;

    if (data.size() < sizeof(SignedProposal)) return;

    SignedProposal sp;
    memcpy(&sp, data.data(), sizeof(SignedProposal));

    std::string role = CryptoAuth::instance().verifyCert(sp.cert);
    bool sigOk = !role.empty() &&
                 CryptoAuth::instance().verifyProposalSignature(
                     sp.cert, sp.proposalBytes, sp.proposalSize,
                     sp.timestampMs, sp.signature, sp.signatureLen);

    if (!role.empty() && sigOk) {
        isPrio = (role == "priority");
        if (isPrio) {
            std::cout << NOW << " [CRYPTO][V" << myId_ << "] BEACON from V" << senderId
                      << " verified as PRIORITY (Emergency_CA)" << std::endl;
        }
    } else if (!role.empty() || !sigOk) {
        std::cout << NOW << " [CRYPTO][V" << myId_ << "] BEACON from V" << senderId
                  << " cert/sig INVALID — treated as normal vehicle" << std::endl;
    }
    memcpy(&proposal, sp.proposalBytes, sizeof(VehicleProposal));

    bool wasNew = (vehicleDB_.find(senderId) == vehicleDB_.end());
    proposal.isPriority = isPrio;  // set from cert, never from payload
    vehicleDB_[senderId] = proposal;

    if (wasNew) {
        std::cout << NOW << " [DBG][V" << myId_ << "] PEER_BEACON: new vehicle V" << senderId
                  << " laneIdx=" << proposal.laneIndex
                  << " dist=" << proposal.distanceToJunction << "m"
                  << " isPriority=" << isPrio
                  << " vehicleDB_=" << vehicleDB_.size() << " total" << std::endl;
    }
}

// ============ LANE LEADER FLAG ============

// Recomputed every check interval (~50ms).
// isLaneLeader_ = true if no vehicle in the same lane has a smaller distanceToJunction.
// Also updates vehicleInFrontOfMe_: the vehicle in this lane immediately ahead of us.
//
// Multi-round trigger: if this vehicle transitions from not-leader to leader while
// it is still stopped at the intersection with a committed schedule from a previous
// round, and there are unscheduled vehicles still waiting, startNewRound() is called.
void RaftAppBase::updateLaneLeaderFlag()
{
    bool wasLaneLeader = isLaneLeader_;  // save before recompute

    // Use TraCI to determine lane leadership: ask SUMO directly whether any vehicle
    // is ahead of us in the same lane.  Real-time and accurate — no stale beacon data.
    isLaneLeader_ = isLaneLeaderByTraci();
    wayOfSight_   = isLaneLeader_;  // no vehicle ahead = clear line of sight

    // Still maintain vehicleInFrontOfMe_ from beacon data — needed by handleVehiclePassed()
    // so it knows which vehicle ID to watch for before advancing in the queue.
    vehicleInFrontOfMe_ = -1;
    if (!isLaneLeader_) {
        double myDist = calculateDistanceToJunction();
        if (myDist < 0) myDist = 999999.0;
        double closestFrontDist = 999999.0;
        for (auto& kv : vehicleDB_) {
            int vid = kv.first;
            const VehicleProposal& prop = kv.second;
            if (vid == myId_) continue;
            if (prop.laneIndex != myLaneIndex_) continue;
            if (prop.distanceToJunction < myDist &&
                prop.distanceToJunction < closestFrontDist) {
                closestFrontDist    = prop.distanceToJunction;
                vehicleInFrontOfMe_ = vid;
            }
        }
    }

    // ---- Multi-round: detect lane-leader promotion ----
    // Conditions:
    //   1. We were not lane leader last tick, but we are now (previous leader left).
    //   2. We are still physically at the intersection (not yet passed).
    //   3. A PASS_ORDER was already committed in a previous round.
    //   4. We are not already in the process of forming a new cluster.
    //   5. There exist vehicles in vehicleDB_ that were not scheduled in the previous round.
    if (!wasLaneLeader && isLaneLeader_ &&
        hasStoppedAtIntersection_ && hasCommittedOrder_ &&
        !hasPassedIntersection_ && !seekingNewCluster_ && !isFallbackMode_) {

        if (hasUnscheduledVehicles()) {
            std::cout << NOW << " [ROUND][V" << myId_ << "] Lane leader promotion detected"
                      << " (prev wasLaneLeader=" << wasLaneLeader << ") — starting new RAFT round." << std::endl;
            startNewRound();
        } else {
            std::cout << NOW << " [ROUND][V" << myId_ << "] Lane leader promotion: all vehicles"
                      << " already scheduled — no new round needed." << std::endl;
        }
    }
}

// ============ MULTI-ROUND: start a fresh RAFT cluster ============
//
// Called when this vehicle becomes the new lane leader after the previous leader passed.
// Resets all RAFT and coordination state so a new election can start fresh.
// scheduledVehicles_ is populated from the committed schedule of the previous round
// (and from any QC_BROADCAST already received) so proposePassOrder() won't re-schedule
// vehicles that already have a committed crossing order.
void RaftAppBase::startNewRound()
{
    seekingNewCluster_ = true;
    roundNumber_++;

    // Build scheduledVehicles_ from our local committed schedule.
    // (QC_BROADCAST may have already populated this; we add our own copy as a safety net.)
    for (int b = 0; b < committedSchedule_.numBatches; b++) {
        for (int v = 0; v < committedSchedule_.batches[b].numVehicles; v++) {
            scheduledVehicles_.insert(committedSchedule_.batches[b].vehicleIds[v]);
        }
    }

    std::cout << NOW << " [ROUND][V" << myId_ << "] === STARTING ROUND " << roundNumber_
              << " === (scheduledVehicles_=" << scheduledVehicles_.size()
              << " vehicleDB_=" << vehicleDB_.size() << ")" << std::endl;

    // Free the previous RAFT server — it served its purpose.
    if (raftServer_) {
        raft_free(raftServer_);
        raftServer_ = nullptr;
    }

    // Reset all RAFT and coordination state for the new round.
    raftStarted_         = false;
    passOrderProposed_   = false;
    hasCommittedOrder_   = false;
    isLeader_            = false;
    wasElectedLeader_    = false;
    waitingForStatus_    = false;
    statusResponseCount_ = 0;
    proposedLeft_.clear();
    vehiclesLeftInBatch_.clear();
    gossipSeenVehicleLeft_.clear();
    currentBatch_        = 0;
    myBatch_             = -1;
    lastAppliedIndex_    = 0;
    failedElectionCount_ = 0;
    lastCheckedTerm_     = 0;
    lastRaftPeriodicRun_ = SIMTIME_ZERO;
    clusterPhase_        = PHASE_DISCOVERY;
    activeVehicles_.clear();
    activeVehicles_.insert(myId_);
    collectedLaneLeaders_.clear();
    collectedLeaderDBs_.clear();

    // Reset QC assembly for the new round.
    qcAssembled_ = false;
    collectedQCSigs_.clear();

    // Reset timing metrics for the new round.
    timeClusterFormed_ = SIMTIME_ZERO;
    timeElected_       = SIMTIME_ZERO;
    timeOrderCommitted_= SIMTIME_ZERO;
    logEntriesProposed_= 0;
    logEntriesCommitted_= 0;
    electionRounds_    = 0;
    totalRaftDecisionTimeSec_ = 0.0;
    statusCollectionTimeMs_   = 0.0;
    coordinationMethod_ = "raft";

    // Wait 500ms for late beacons, then start cluster formation loop.
    scheduleOneshotMs(500.0, [this]() {
        seekingNewCluster_ = false;
        if (!hasPassedIntersection_ && !raftStarted_) {
            std::cout << NOW << " [ROUND][V" << myId_ << "] Round " << roundNumber_
                      << " discovery window elapsed — attempting cluster formation." << std::endl;
            sendLaneLeaderBeacon();
            scheduleClusterFormationLoop();
        }
    });
}

// ============ CLUSTER FORMATION — BROADCAST APPROACH ============
//
// Each lane leader broadcasts CLUSTER_JOIN_INVITE carrying [myId, myLaneIndex].
// Every receiving lane leader stores it in collectedLaneLeaders_[laneIndex] = vehicleId.
// Self is added immediately on send.
// Once all approach lanes are represented → formCluster() with the collected IDs.
// A 500ms retry loop keeps broadcasting until raftStarted_.

// Broadcast own lane-leader announcement and check if cluster is complete.
void RaftAppBase::sendLaneLeaderBeacon()
{
    if (raftStarted_ || hasPassedIntersection_ || !isLaneLeader_) return;

    // Register self
    collectedLaneLeaders_[myLaneIndex_] = myId_;

    // Payload: [vehicleId:4B][laneIndex:4B]
    std::vector<uint8_t> data(sizeof(int) * 2);
    memcpy(data.data(),              &myId_,        sizeof(int));
    memcpy(data.data() + sizeof(int), &myLaneIndex_, sizeof(int));
    sendRaftBroadcast(/*CLUSTER_JOIN_INVITE*/ 0x15, data);
    messagesSent_++;

    int numLanes = (int)approachEdgeList_.size();
    std::cout << NOW << " [DBG][V" << myId_ << "] LANE_LEADER_BEACON sent"
              << " laneIdx=" << myLaneIndex_
              << " collected=" << collectedLaneLeaders_.size() << "/" << numLanes << std::endl;

    tryFormClusterFromCollected();
}

// Check if all lane leaders have announced — if so, form the cluster.
// Also broadcasts CLUSTER_FORM_BROADCAST so every member can independently call formCluster().
void RaftAppBase::tryFormClusterFromCollected()
{
    if (raftStarted_) return;
    int numLanes = (int)approachEdgeList_.size();
    if ((int)collectedLaneLeaders_.size() < numLanes) return;

    std::set<int> members;
    for (auto& kv : collectedLaneLeaders_) members.insert(kv.second);

    std::cout << NOW << " [DBG][V" << myId_ << "] ALL LANES PRESENT — forming cluster ["
              << members.size() << "]: [";
    for (int v : members) std::cout << v << " ";
    std::cout << "]" << std::endl;

    // Broadcast member list so every member calls formCluster() with the same set.
    // Payload: [numMembers:4B][vehicleId:4B * N]
    int numMembers = (int)members.size();
    std::vector<uint8_t> bcastData(sizeof(int) * (1 + numMembers));
    memcpy(bcastData.data(), &numMembers, sizeof(int));
    int idx = 0;
    for (int vid : members) {
        memcpy(bcastData.data() + sizeof(int) * (1 + idx), &vid, sizeof(int));
        idx++;
    }
    sendRaftBroadcast(/*CLUSTER_FORM_BROADCAST*/ 0x16, bcastData);
    messagesSent_++;

    raftStarted_ = true;
    formCluster(members);
}

// Receive CLUSTER_FORM_BROADCAST: extract member list and call formCluster() if not yet started.
void RaftAppBase::handleClusterFormBroadcast(const std::vector<uint8_t>& data)
{
    if (raftStarted_ || hasPassedIntersection_) return;
    if (data.size() < sizeof(int)) return;

    int numMembers;
    memcpy(&numMembers, data.data(), sizeof(int));
    if (numMembers <= 0 || (int)data.size() < (int)sizeof(int) * (1 + numMembers)) return;

    std::set<int> members;
    for (int i = 0; i < numMembers; i++) {
        int vid;
        memcpy(&vid, data.data() + sizeof(int) * (1 + i), sizeof(int));
        members.insert(vid);
    }

    // Only join if we are one of the members
    if (members.count(myId_) == 0) return;

    std::cout << NOW << " [DBG][V" << myId_ << "] CLUSTER_FORM_BROADCAST: joining cluster ["
              << members.size() << "]: [";
    for (int v : members) std::cout << v << " ";
    std::cout << "]" << std::endl;

    raftStarted_ = true;
    formCluster(members);
}

// Repeating 500ms loop: keep broadcasting until RAFT starts.
void RaftAppBase::scheduleClusterFormationLoop()
{
    scheduleOneshotMs(500.0, [this]() {
        if (!raftStarted_ && !hasPassedIntersection_) {
            if (isLaneLeader_ && hasStoppedAtIntersection_)
                sendLaneLeaderBeacon();
            scheduleClusterFormationLoop();
        }
    });
}

// Receive a lane leader announcement.
// If we are a lane leader: store it and check if cluster is complete.
// Non-lane-leaders ignore it — they wait for COORD_PASS_ORDER_BROADCAST.
void RaftAppBase::handleClusterJoinInvite(const std::vector<uint8_t>& data, int senderId)
{
    if (raftStarted_ || hasPassedIntersection_) return;
    if (data.size() < sizeof(int) * 2) return;

    int vehicleId, laneIndex;
    memcpy(&vehicleId,  data.data(),              sizeof(int));
    memcpy(&laneIndex,  data.data() + sizeof(int), sizeof(int));

    if (vehicleId != senderId) return;  // sanity check
    if (laneIndex < 0 || laneIndex >= (int)approachEdgeList_.size()) return;

    collectedLaneLeaders_[laneIndex] = vehicleId;

    std::cout << NOW << " [DBG][V" << myId_ << "] LANE_LEADER_BEACON from V" << senderId
              << " lane=" << laneIndex
              << " collected=" << collectedLaneLeaders_.size()
              << "/" << approachEdgeList_.size() << std::endl;

    // Only lane leaders trigger cluster formation
    if (isLaneLeader_) {
        collectedLaneLeaders_[myLaneIndex_] = myId_;  // ensure self is registered
        tryFormClusterFromCollected();
    }
}

// ============ RAFT CLUSTER FORMATION ============

// Initialise the raft_server_t with the agreed member set.
// Called by sendClusterJoinInvite (lane leader) or handleClusterJoinInvite (member).
void RaftAppBase::formCluster(const std::set<int>& members)
{
    if (clusterPhase_ != PHASE_DISCOVERY) return;  // already formed

    clusterPhase_      = PHASE_FORMATION;
    timeClusterFormed_ = NOW;
    activeVehicles_    = members;

    std::cout << NOW << " [DBG][V" << myId_ << "] FORM_CLUSTER " << members.size()
              << " members: [";
    for (int v : members) std::cout << v << " ";
    std::cout << "] electionBase=" << electionTimeoutBaseMs_ << "ms" << std::endl;

    // Initialise RAFT server
    raft_server_t* s = raft_new();
    if (!s) {
        RAFT_LOG_ERR("raft_new() returned nullptr!");
        return;
    }
    raftServer_ = s;

    raft_cbs_t cbs;
    memset(&cbs, 0, sizeof(cbs));
    // These are teh functions that will be called by the RAFT library when it needs to send messages, persist logs, etc.
    // This is how we stay protocol agnostic
    cbs.send_requestvote      = &RaftAppBase::sendRequestVote;
    cbs.send_appendentries    = &RaftAppBase::sendAppendEntries;
    cbs.log_offer             = &RaftAppBase::logOffer;
    cbs.applylog              = &RaftAppBase::applylog;
    cbs.log                   = &RaftAppBase::raftLog;
    cbs.persist_vote          = &RaftAppBase::persistVote;
    raft_set_callbacks(raftServer_, &cbs, this);

    int electionTimeout = electionTimeoutBaseMs_ + (int)getRandomDouble(0.0, (double)electionTimeoutJitterMs_);
    raft_set_election_timeout(raftServer_, electionTimeout);
    raft_set_request_timeout(raftServer_, requestTimeoutMs_);

    myRaftNodeId_ = getNodeIdFromVehicleId(myId_);

    for (int vid : members) {
        raft_node_id_t nodeId = getNodeIdFromVehicleId(vid);
        void* udata = reinterpret_cast<void*>(static_cast<intptr_t>(vid));
        raft_node_t* node = raft_add_node(raftServer_, udata, nodeId, (vid == myId_) ? 1 : 0);
        if (!node) {
            RAFT_LOG_ERR("Failed to add Raft node for vehicle " << vid);
        }
    }

    // Mark inactive any vehicles that left before we formed
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
    onClusterFormed();
}
