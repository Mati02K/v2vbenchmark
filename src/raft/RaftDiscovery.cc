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
    collectedProposals_.clear();
    collectedWayOfSight_.clear();
    proposedLeft_.clear();
    vehiclesLeftInBatch_.clear();
    gossipSeenVehicleLeft_.clear();
    committedStatuses_.clear();
    currentBatch_        = 0;
    myBatch_             = -1;
    lastAppliedIndex_    = 0;
    failedElectionCount_ = 0;
    lastCheckedTerm_     = 0;
    lastRaftPeriodicRun_ = SIMTIME_ZERO;
    clusterPhase_        = PHASE_DISCOVERY;
    activeVehicles_.clear();
    activeVehicles_.insert(myId_);
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
            tryFormClusterFromVehicleDB();
            scheduleClusterFormationLoop();
        }
    });
}

// ============ CLUSTER FORMATION FROM vehicleDB_ ============

// Compute the lane leaders from our own vehicleDB_ and send CLUSTER_JOIN_INVITE.
// One lane leader per approach lane = vehicle with smallest distanceToJunction in that lane.
// Called once after the discovery wait and retried every 500ms until raftStarted_.
void RaftAppBase::tryFormClusterFromVehicleDB()
{
    if (raftStarted_ || hasPassedIntersection_ || !isLaneLeader_) return;

    // Ensure self is current in vehicleDB_
    vehicleDB_[myId_] = updateMyProposal();

    int numLanes = (int)approachEdgeList_.size();

    // For each approach lane, find the vehicle with smallest distanceToJunction.
    std::map<int, std::pair<int, double>> bestPerLane;  // laneIndex → {vehicleId, dist}
    for (auto& kv : vehicleDB_) {
        const VehicleProposal& p = kv.second;
        int lane = p.laneIndex;
        if (lane < 0 || lane >= numLanes) continue;
        double dist = p.distanceToJunction;
        if (!bestPerLane.count(lane) || dist < bestPerLane[lane].second) {
            bestPerLane[lane] = {kv.first, dist};
        }
    }

    std::set<int> laneLeaderIds;
    for (auto& kv : bestPerLane) laneLeaderIds.insert(kv.second.first);
    laneLeaderIds.insert(myId_);  // always include self

    std::cout << NOW << " [DBG][V" << myId_ << "] TRY_FORM_CLUSTER: vehicleDB_=" << vehicleDB_.size()
              << " lanes=" << bestPerLane.size() << "/" << numLanes
              << " laneLeaders=[";
    for (int v : laneLeaderIds) std::cout << v << " ";
    std::cout << "]" << std::endl;

    if ((int)bestPerLane.size() < numLanes) {
        std::cout << NOW << " [DBG][V" << myId_ << "] TRY_FORM_CLUSTER: only "
                  << bestPerLane.size() << "/" << numLanes
                  << " lanes visible — will retry." << std::endl;
        return;  // retry loop will call again in 500ms
    }

    sendClusterJoinInvite(laneLeaderIds);
}

// Repeating 500ms loop: keep trying to form cluster until RAFT starts.
void RaftAppBase::scheduleClusterFormationLoop()
{
    scheduleOneshotMs(500.0, [this]() {
        if (!raftStarted_ && !hasPassedIntersection_) {
            if (isLaneLeader_ && hasStoppedAtIntersection_) {
                tryFormClusterFromVehicleDB();
            }
            scheduleClusterFormationLoop();
        }
    });
}

// Lane leader sends unicast CLUSTER_JOIN_INVITE to each member and forms cluster itself.
// Payload: [numMembers:1B][vehicleId * numMembers (4B each)]
void RaftAppBase::sendClusterJoinInvite(const std::set<int>& members)
{
    if (raftStarted_) return;
    raftStarted_ = true;  // set before formCluster to block re-entry

    uint8_t num = static_cast<uint8_t>(std::min(members.size(), (size_t)255));
    std::vector<uint8_t> data(1 + num * sizeof(int));
    data[0] = num;
    int i = 0;
    for (int v : members) {
        memcpy(data.data() + 1 + i * sizeof(int), &v, sizeof(int));
        i++;
    }

    // Unicast to each member except self
    for (int v : members) {
        if (v != myId_) {
            sendRaftToPeer(v, /*CLUSTER_JOIN_INVITE*/ 0x15, data);
            messagesSent_++;
        }
    }

    std::cout << NOW << " [DBG][V" << myId_ << "] CLUSTER_JOIN_INVITE sent to "
              << (members.size() - 1) << " peers. Forming cluster." << std::endl;

    formCluster(members);
}

// Non-leader vehicle receives CLUSTER_JOIN_INVITE and joins RAFT cluster.
void RaftAppBase::handleClusterJoinInvite(const std::vector<uint8_t>& data, int senderId)
{
    if (raftStarted_) return;  // already formed (e.g., got invite from another lane leader)
    if (data.size() < 1) return;

    uint8_t num = data[0];
    if (data.size() < 1 + num * sizeof(int)) return;

    std::set<int> members;
    for (int i = 0; i < num; i++) {
        int vid;
        memcpy(&vid, data.data() + 1 + i * sizeof(int), sizeof(int));
        if (vid >= 0 && vid < totalVehicles_) members.insert(vid);
    }

    if (members.empty()) return;

    std::cout << NOW << " [DBG][V" << myId_ << "] CLUSTER_JOIN_INVITE from V" << senderId
              << " — joining RAFT cluster of " << members.size() << " members: [";
    for (int v : members) std::cout << v << " ";
    std::cout << "]" << std::endl;

    raftStarted_ = true;
    formCluster(members);
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
