// RaftDiscovery.cc — From simulation start to vehicle stopped at intersection
//                    and RAFT cluster formed.
//
// Protocol phases covered:
//   1. DISCOVERY  — All vehicles broadcast PEER_BEACON, build vehicleDB_
//   2. FORMATION  — Lane leaders announce, cluster forms, RAFT initialised
//   3. STOP       — Per-tick intersection stop detection, queue management
//
// After formCluster() returns, RaftCore.cc takes over.

#include "raft/RaftAppBase.h"
#include "raft/RaftTypes_m.h"

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
    sendRaftBroadcast(benchmark::PEER_BEACON, data);
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

    isPrio = verifySignedProposal(sp, senderId);
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

    // Still maintain vehicleInFrontOfMe_ from beacon data — needed by handleVehicleLeft()
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
    currentBatch_        = 0;
    myBatch_             = -1;
    lastAppliedIndex_    = 0;
    lastCheckedTerm_     = 0;
    lastRaftPeriodicRun_ = SIMTIME_ZERO;
    clusterPhase_        = PHASE_DISCOVERY;
    activeVehicles_.clear();
    activeVehicles_.insert(myId_);
    collectedLaneLeaders_.clear();
    collectedAllVehicles_.clear();
    collectedLeaderDBs_.clear();

    // Reset QC assembly for the new round.
    qcAssembled_ = false;
    collectedQCSigs_.clear();

    // Reset timing metrics for the new round.
    timeRaftStarted_    = SIMTIME_ZERO;
    logEntriesProposed_ = 0;
    logEntriesCommitted_= 0;
    electionRounds_     = 0;
    totalRaftTimeSec_   = 0.0;
    coordinationMethod_ = "raft";

    // Wait 500ms for late beacons, then start cluster formation loop.
    scheduleOneshotMs(500.0, [this]() {
        seekingNewCluster_ = false;
        if (!hasPassedIntersection_ && !raftStarted_) {
            std::cout << NOW << " [ROUND][V" << myId_ << "] Round " << roundNumber_
                      << " discovery window elapsed — attempting cluster formation." << std::endl;
            sendClusterBeacon();
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

// Broadcast cluster-readiness announcement and check if cluster is complete.
// In laneLeaders mode: only lane leaders call this.
// In allVehicles mode: every stopped vehicle calls this.
void RaftAppBase::sendClusterBeacon()
{
    if (raftStarted_ || hasPassedIntersection_) return;
    if (clusterMode_ == "laneLeaders" && !isLaneLeader_) return;

    // Register self
    collectedLaneLeaders_[myLaneIndex_] = myId_;
    collectedAllVehicles_.insert(myId_);

    // Payload: [vehicleId:4B][laneIndex:4B]
    std::vector<uint8_t> data(sizeof(int) * 2);
    memcpy(data.data(),              &myId_,        sizeof(int));
    memcpy(data.data() + sizeof(int), &myLaneIndex_, sizeof(int));
    sendRaftBroadcast(benchmark::CLUSTER_JOIN_INVITE, data);

    if (clusterMode_ == "allVehicles") {
        std::cout << NOW << " [DBG][V" << myId_ << "] CLUSTER_BEACON sent"
                  << " laneIdx=" << myLaneIndex_
                  << " collected=" << collectedAllVehicles_.size() << "/" << totalVehicles_ << std::endl;
    } else {
        int numLanes = (int)approachEdgeList_.size();
        std::cout << NOW << " [DBG][V" << myId_ << "] CLUSTER_BEACON sent"
                  << " laneIdx=" << myLaneIndex_
                  << " collected=" << collectedLaneLeaders_.size() << "/" << numLanes << std::endl;
    }

    tryFormClusterFromCollected();
}

// Check if enough vehicles have announced — if so, form the cluster.
// In laneLeaders mode: triggers when all lanes have a leader.
// In allVehicles mode: triggers when all totalVehicles_ have announced.
// Also broadcasts CLUSTER_FORM_BROADCAST so every member can independently call formCluster().
void RaftAppBase::tryFormClusterFromCollected()
{
    if (raftStarted_) return;

    std::set<int> members;
    if (clusterMode_ == "allVehicles") {
        if ((int)collectedAllVehicles_.size() < totalVehicles_) return;
        members = collectedAllVehicles_;
    } else {
        int numLanes = (int)approachEdgeList_.size();
        if ((int)collectedLaneLeaders_.size() < numLanes) return;
        for (auto& kv : collectedLaneLeaders_) members.insert(kv.second);
    }

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
    sendRaftBroadcast(benchmark::CLUSTER_FORM_BROADCAST, bcastData);

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
            if ((clusterMode_ == "allVehicles" || isLaneLeader_) && hasStoppedAtIntersection_)
                sendClusterBeacon();
            scheduleClusterFormationLoop();
        }
    });
}

// Receive a cluster-readiness announcement.
// In laneLeaders mode: only lane leaders store and trigger formation.
// In allVehicles mode: every vehicle stores and triggers formation.
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
    collectedAllVehicles_.insert(vehicleId);

    if (clusterMode_ == "allVehicles") {
        std::cout << NOW << " [DBG][V" << myId_ << "] CLUSTER_BEACON from V" << senderId
                  << " lane=" << laneIndex
                  << " collected=" << collectedAllVehicles_.size()
                  << "/" << totalVehicles_ << std::endl;
    } else {
        std::cout << NOW << " [DBG][V" << myId_ << "] CLUSTER_BEACON from V" << senderId
                  << " lane=" << laneIndex
                  << " collected=" << collectedLaneLeaders_.size()
                  << "/" << approachEdgeList_.size() << std::endl;
    }

    if (clusterMode_ == "allVehicles" || isLaneLeader_) {
        if (clusterMode_ == "laneLeaders")
            collectedLaneLeaders_[myLaneIndex_] = myId_;  // ensure self is registered
        tryFormClusterFromCollected();
    }
}

// ============ RAFT CLUSTER FORMATION ============

// Initialise the raft_server_t with the agreed member set.
// Called by tryFormClusterFromCollected (lane leader) or handleClusterFormBroadcast (member).
void RaftAppBase::formCluster(const std::set<int>& members)
{
    if (clusterPhase_ != PHASE_DISCOVERY) return;  // already formed

    clusterPhase_    = PHASE_FORMATION;
    timeRaftStarted_ = NOW;
    activeVehicles_  = members;

    std::cout << NOW << " [DBG][V" << myId_ << "] FORM_CLUSTER " << members.size()
              << " members: [";
    for (int v : members) std::cout << v << " ";
    std::cout << "] electionBase=" << electionTimeoutBaseMs_ << "ms" << std::endl;

    // Initialise RAFT server
    raft_server_t* s = raft_new();
    if (!s) {
        std::cerr << "Vehicle " << myId_ << " raft_new() returned nullptr!" << std::endl;
        return;
    }
    raftServer_ = s;

    raft_cbs_t cbs;
    memset(&cbs, 0, sizeof(cbs));
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
            std::cerr << "Vehicle " << myId_ << " failed to add Raft node for vehicle " << vid << std::endl;
        }
    }

    // Mark inactive any vehicles that left before we formed
    for (int vid : vehiclesLeftBeforeFormed_) {
        if (vid == myId_) continue;
        raft_node_t* node = raft_get_node(raftServer_, getNodeIdFromVehicleId(vid));
        if (node) {
            raft_node_set_active(node, 0);
            std::cout << simTime() << " [DBG][V" << myId_ << "] RAFT node V" << vid << " marked INACTIVE (left before cluster formed)" << std::endl;
        }
    }
    vehiclesLeftBeforeFormed_.clear();

    clusterPhase_ = PHASE_COORDINATION;
    onClusterFormed();
}

// ============ INTERSECTION STOP DETECTION ============

// Called once the first time a vehicle stops at the intersection.
// Triggers leader DB exchange (if lane leader) and the fallback timeout.
void RaftAppBase::onFirstStoppedAtIntersection()
{
    updateLaneLeaderFlag();

    std::cout << NOW << " [DBG][V" << myId_ << "] FIRST_STOP:"
              << " isLaneLeader=" << isLaneLeader_
              << " vehicleDB_=" << vehicleDB_.size()
              << " laneIdx=" << myLaneIndex_ << std::endl;

    // Print vehicles currently known to this vehicle
    std::set<int> distinctLanes;
    for (const auto& kv : vehicleDB_)
        distinctLanes.insert(kv.second.laneIndex);

    std::cout << NOW << " [VIEW][V" << myId_ << "] vehicles in view: [";
    bool first = true;
    for (const auto& kv : vehicleDB_) {
        if (!first) std::cout << ", ";
        std::cout << kv.first;
        first = false;
    }
    std::cout << "] LanesInView: " << distinctLanes.size() << std::endl;

    if (clusterMode_ == "allVehicles" || isLaneLeader_) {
        sendClusterBeacon();
        scheduleClusterFormationLoop();
    }

    // Fallback: if RAFT cluster hasn't formed within fallbackClusterTimeoutMs_, activate fallback
    scheduleOneshotMs((double)fallbackClusterTimeoutMs_, [this]() {
        if (!hasPassedIntersection_ && !hasCommittedOrder_) {
            std::cout << NOW << " [WARN][V" << myId_ << "] FALLBACK TIMEOUT (" << fallbackClusterTimeoutMs_
                      << "ms): no committed order. vehicleDB_=" << vehicleDB_.size() << std::endl;
            handleFallback();
        }
    });
}

// ---- updateRoadTracking -----------------------------------------------------
// Detects road changes, logs them, and proactively caches the approach edge +
// lane index so cluster-junction detection can use them even if the approach
// edge was too short to trigger a stop check.
void RaftAppBase::updateRoadTracking(const std::string& roadId, const std::string& lastKnownRoad)
{
    if (roadId == lastKnownRoad) {
        // Periodic position debug (every 5s)
        if (NOW - lastStopDebugPrint_ > 5.0) {
            lastStopDebugPrint_ = NOW;
            double spd = 999.0; try { spd = traciVehicle_->getSpeed(); } catch (...) {}
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
        return;
    }
    prevRoadId_ = roadId;
    double spd = 999.0; try { spd = traciVehicle_->getSpeed(); } catch (...) {}
    std::cout << simTime() << " [DBG][V" << myId_ << "] ROAD_CHANGE -> " << roadId
              << " dist=" << getDistanceToJunction() << "m speed=" << spd << "m/s"
              << " onApproach=" << (intersectionEdges_.count(roadId) ? "YES" : "NO") << std::endl;
    if (intersectionEdges_.count(roadId)) {
        intersectionEdge_ = roadId;
        for (int i = 0; i < (int)approachEdgeList_.size(); i++) {
            if (approachEdgeList_[i] == roadId) { myLaneIndex_ = i; break; }
        }
    }
}

// ---- handleClusterJunctionStop ----------------------------------------------
// Handles vehicles that entered the junction internal road without being caught
// on the approach edge (very short approach lane traversed between timer ticks).
void RaftAppBase::handleClusterJunctionStop(const std::string& roadId, const std::string& lastKnownRoad)
{
    if (hasStoppedAtIntersection_) return;

    // Recover approach edge from lastKnownRoad if not already cached.
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
              << " road=" << roadId << " approachEdge=" << intersectionEdge_
              << " laneIdx=" << myLaneIndex_ << " wos=" << wayOfSight_
              << " isLeader=" << isLeader_ << std::endl;
    onFirstStoppedAtIntersection();
}

// ---- handleFrontStop --------------------------------------------------------
// Vehicle is at the front of the queue (within stop distance).
// First time: records stop, decides whether to go through or wait.
// Subsequent ticks: re-applies hard stop if still waiting.
void RaftAppBase::handleFrontStop(const std::string& roadId, double dist)
{
    if (!hasStoppedAtIntersection_) {
        hasStoppedAtIntersection_ = true;
        intersectionEdge_ = roadId;
        calculateWayOfSight();
        onFirstStoppedAtIntersection();

        bool alreadyMoving = (timeStartedMoving_ != SIMTIME_ZERO);
        bool canGoNow = !alreadyMoving && hasCommittedOrder_
                     && myBatch_ != -1 && myBatch_ <= currentBatch_
                     && !hasPassedIntersection_;

        if (alreadyMoving || canGoNow) {
            std::cout << simTime() << " [NO_STOP][V" << myId_ << "] batch=" << myBatch_
                      << " curBatch=" << currentBatch_
                      << " alreadyMoving=" << alreadyMoving << " -> skip stop" << std::endl;
            if (!alreadyMoving) {
                resumeMovement();
            }
        } else {
            stopVehicle();
            std::cout << simTime() << " [STOP_CHECK][V" << myId_ << "] STOPPED (front) dist="
                      << dist << "m batch=" << myBatch_ << " curBatch=" << currentBatch_
                      << " committed=" << hasCommittedOrder_
                      << " isLeader=" << isLeader_ << std::endl;
        }
    } else {
        // Re-apply hard stop if still waiting (prevents SUMO creep).
        if (!hasCommittedOrder_ || (myBatch_ > currentBatch_ && timeStartedMoving_ == SIMTIME_ZERO))
            stopVehicle();
    }
}

// ---- handleQueuedStop -------------------------------------------------------
// Vehicle is queued behind the front vehicle (further back, nearly stopped).
// SUMO car-following holds its position naturally — no hard stop needed.
void RaftAppBase::handleQueuedStop(const std::string& roadId, double dist, double speed)
{
    hasStoppedAtIntersection_ = true;
    intersectionEdge_ = roadId;
    calculateWayOfSight();
    std::cout << simTime() << " [STOP_CHECK][V" << myId_ << "] QUEUED (no hard stop) dist=" << dist
              << "m speed=" << speed << "m/s road=" << roadId
              << " wos=" << wayOfSight_ << " frontVeh=" << vehicleInFrontOfMe_
              << " isLeader=" << isLeader_ << std::endl;
    onFirstStoppedAtIntersection();
}

// ---- checkAndStopAtIntersection ---------------------------------------------
// Called on every timer tick. Coordinates when/whether to stop at the junction.
void RaftAppBase::checkAndStopAtIntersection()
{
    if (hasPassedIntersection_ || !traciVehicle_) return;

    // Record first time vehicle enters the intersection zone, even if already moving.
    if (timeStopped_ == SIMTIME_ZERO) {
        try {
            double dist = getDistanceToJunction();
            if (dist >= 0 && dist < intersectionStopDistance_ * (totalVehicles_ / 2.0)) {
                timeStopped_ = NOW;
            }
        } catch (...) {}
    }

    if (timeStartedMoving_ > SIMTIME_ZERO) return;

    try {
        std::string roadId      = traciVehicle_->getRoadId();
        std::string lastKnownRoad = prevRoadId_;
        updateRoadTracking(roadId, lastKnownRoad);

        // Vehicle entered the junction internal road directly (short approach lane).
        bool onClusterJunction = (!roadId.empty() && roadId[0] == ':' &&
                                   roadId.find("cluster") != std::string::npos);
        if (onClusterJunction) {
            handleClusterJunctionStop(roadId, lastKnownRoad);
            return;
        }

        if (!intersectionEdges_.count(roadId)) return;

        double dist  = getDistanceToJunction();
        double speed = 999.0;
        try { speed = traciVehicle_->getSpeed(); } catch (...) {}

        bool closeEnough = (dist >= 0 && dist <= intersectionStopDistance_);
        bool queued      = (!closeEnough && speed < 1.5 &&
                            dist >= 0 && dist < intersectionStopDistance_ * (totalVehicles_ / 2.0) &&
                            !hasStoppedAtIntersection_);

        if (closeEnough) {
            handleFrontStop(roadId, dist);
        } else if (queued) {
            handleQueuedStop(roadId, dist, speed);
        }

    } catch (...) {}
}

// TraCI-based queue advancement: on every check-timer tick, ask SUMO who is ahead
// and how far. When the gap exceeds the stop distance (leader moved away), hand
// control back to SUMO car-following so the whole queue accordions forward.
// Once the vehicle reaches the stop line it is re-caught by checkAndStopAtIntersection.
void RaftAppBase::checkAndAdvanceInQueue()
{
    if (!traciVehicle_) return;
    try {
        double speed = traciVehicle_->getSpeed();
        if (speed > 0.5) return;  // already moving under SUMO

        double dist = getDistanceToJunction();
        if (dist < 0 || dist <= intersectionStopDistance_) return;  // handled by checkAndStop

        // Ask TraCI for the leading vehicle and gap ahead
        auto leader = traciVehicle_->getLeader(intersectionStopDistance_ * (totalVehicles_ / 2.0));

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
            std::cout << simTime() << " [QUEUE_ADV][V" << myId_ << "] RELEASING gap=" << leader.second
                      << "m dist=" << dist << "m leader=" << leader.first << std::endl;
            traciVehicle_->setSpeedMode(31);  // all SUMO safety checks on
            traciVehicle_->setSpeed(-1);       // SUMO car-following takes over
        }
    } catch (...) {}
}
