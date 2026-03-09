// RaftDiscovery.cc — Pre-intersection discovery, lane-leader election,
//                    intersection leader DB exchange, and RAFT cluster formation.
//
// Protocol phases:
//   1. DISCOVERY: All vehicles broadcast PEER_BEACON (own VehicleProposal).
//      All vehicles build vehicleDB_[senderId] = proposal.
//      isLaneLeader_ flag is recomputed every check interval.
//
//   2. INTERSECTION: When stopped, lane leaders broadcast LEADER_DB_EXCHANGE
//      (their full vehicleDB_). All vehicles collect leader DBs and, once all
//      approach-lane leaders are heard, compute the RAFT cluster membership
//      (strict intersection, with all lane leaders force-included) and send
//      unicast CLUSTER_JOIN_INVITE to each member.
//
//   3. RAFT: formCluster() initialises the raft_server_t with the agreed member
//      set and triggers election. Everything after this is handled by RaftCore.cc.

#include "raft/RaftAppBase.h"
#include "raft/RaftLogger.h"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <set>

#define NOW (simTime())

// ============ PRE-INTERSECTION PEER BEACON ============

// Broadcast own VehicleProposal so all peers can build vehicleDB_.
void RaftAppBase::sendPeerBeacon()
{
    if (hasPassedIntersection_) return;

    VehicleProposal myProposal = buildMyProposal();
    vehicleDB_[myId_] = myProposal;  // keep self entry up-to-date

    std::vector<uint8_t> data(sizeof(VehicleProposal));
    memcpy(data.data(), &myProposal, sizeof(VehicleProposal));
    sendRaftBroadcast(/*PEER_BEACON*/ 0x10, data);
    messagesSent_++;
}

// Receive a PEER_BEACON and store the sender's proposal.
void RaftAppBase::handlePeerBeacon(const std::vector<uint8_t>& data, int senderId)
{
    if (senderId == myId_) return;
    if (senderId < 0 || senderId >= totalVehicles_) return;
    if (data.size() < sizeof(VehicleProposal)) return;

    VehicleProposal proposal;
    memcpy(&proposal, data.data(), sizeof(VehicleProposal));
    bool wasNew = (vehicleDB_.find(senderId) == vehicleDB_.end());
    vehicleDB_[senderId] = proposal;

    if (wasNew) {
        std::cout << NOW << " [DBG][V" << myId_ << "] PEER_BEACON: new vehicle V" << senderId
                  << " laneIdx=" << proposal.laneIndex
                  << " dist=" << proposal.distanceToJunction << "m"
                  << " vehicleDB_=" << vehicleDB_.size() << " total" << std::endl;
    }
}

// ============ LANE LEADER FLAG ============

// Recomputed every check interval (~50ms).
// isLaneLeader_ = true if no vehicle in the same lane has a smaller distanceToJunction.
// Also updates vehicleInFrontOfMe_: the vehicle in this lane immediately ahead of us.
void RaftAppBase::updateLaneLeaderFlag()
{
    double myDist = calculateDistanceToJunction();
    if (myDist < 0) myDist = 999999.0;

    isLaneLeader_       = true;
    vehicleInFrontOfMe_ = -1;
    double closestFrontDist = 999999.0;

    for (auto& kv : vehicleDB_) {
        int vid = kv.first;
        const VehicleProposal& prop = kv.second;
        if (vid == myId_) continue;
        if (prop.laneIndex != myLaneIndex_) continue;

        double theirDist = prop.distanceToJunction;
        if (theirDist < myDist) {
            isLaneLeader_ = false;
            if (theirDist < closestFrontDist) {
                closestFrontDist    = theirDist;
                vehicleInFrontOfMe_ = vid;
            }
        }
    }

    wayOfSight_ = (vehicleInFrontOfMe_ == -1);
}

// ============ INTERSECTION LEADER DB EXCHANGE ============

// Lane leader broadcasts its full vehicleDB_ at the intersection.
// Payload: [senderLaneIndex:1B][numEntries:2B][VehicleProposal * numEntries]
void RaftAppBase::sendLeaderDbExchange()
{
    if (!isLaneLeader_ || !hasStoppedAtIntersection_ || raftStarted_ || hasPassedIntersection_)
        return;

    // Ensure self is in vehicleDB_
    vehicleDB_[myId_] = buildMyProposal();

    uint16_t numEntries = static_cast<uint16_t>(vehicleDB_.size());
    std::vector<uint8_t> data(1 + sizeof(uint16_t) + numEntries * sizeof(VehicleProposal));
    size_t off = 0;
    data[off++] = static_cast<uint8_t>(myLaneIndex_);
    memcpy(data.data() + off, &numEntries, sizeof(uint16_t)); off += sizeof(uint16_t);
    for (auto& kv : vehicleDB_) {
        memcpy(data.data() + off, &kv.second, sizeof(VehicleProposal));
        off += sizeof(VehicleProposal);
    }
    sendRaftBroadcast(/*LEADER_DB_EXCHANGE*/ 0x14, data);
    messagesSent_++;

    // Self-register so we count our own lane as "received"
    receivedLeaderDBs_[myLaneIndex_]       = vehicleDB_;
    receivedLeaderSenderIds_[myLaneIndex_] = myId_;

    std::cout << NOW << " [DBG][V" << myId_ << "] LEADER_DB_EXCHANGE sent"
              << " laneIdx=" << myLaneIndex_
              << " numEntries=" << numEntries
              << ". Now have " << receivedLeaderDBs_.size()
              << "/" << approachEdgeList_.size() << " leader DBs." << std::endl;

    int numLanes = (int)approachEdgeList_.size();
    if ((int)receivedLeaderDBs_.size() >= numLanes)
        tryFormRaftFromLeaderDBs();
}

// Repeating 500ms loop: keep broadcasting leader DB until RAFT starts.
void RaftAppBase::scheduleLeaderDbExchangeLoop()
{
    scheduleOneshotMs(500.0, [this]() {
        if (!raftStarted_ && !hasPassedIntersection_) {
            if (isLaneLeader_ && hasStoppedAtIntersection_)
                sendLeaderDbExchange();
            scheduleLeaderDbExchangeLoop();
        }
    });
}

// Receive a leader DB exchange message. Store under the sender's laneIndex.
// When all approach-lane leader DBs are received, compute intersection and form RAFT.
void RaftAppBase::handleLeaderDbExchange(const std::vector<uint8_t>& data, int senderId)
{
    if (raftStarted_ || hasPassedIntersection_) return;
    if (data.size() < 1 + sizeof(uint16_t)) return;

    size_t off = 0;
    uint8_t senderLaneIndex = data[off++];
    uint16_t numEntries;
    memcpy(&numEntries, data.data() + off, sizeof(uint16_t)); off += sizeof(uint16_t);

    if (data.size() < off + numEntries * sizeof(VehicleProposal)) {
        std::cout << NOW << " [WARN][V" << myId_ << "] LEADER_DB_EXCHANGE from V" << senderId
                  << " truncated (got " << data.size() << " need "
                  << off + numEntries * sizeof(VehicleProposal) << ")" << std::endl;
        return;
    }

    std::map<int, VehicleProposal> leaderDb;
    for (int i = 0; i < numEntries; i++) {
        VehicleProposal prop;
        memcpy(&prop, data.data() + off, sizeof(VehicleProposal));
        off += sizeof(VehicleProposal);
        if (prop.vehicleId >= 0 && prop.vehicleId < totalVehicles_)
            leaderDb[prop.vehicleId] = prop;
    }

    // Also merge into our own vehicleDB_ (best-effort knowledge sharing)
    for (auto& kv : leaderDb) {
        if (vehicleDB_.find(kv.first) == vehicleDB_.end())
            vehicleDB_[kv.first] = kv.second;
    }

    receivedLeaderDBs_[senderLaneIndex]    = leaderDb;
    receivedLeaderSenderIds_[senderLaneIndex] = senderId;

    int numLanes = (int)approachEdgeList_.size();
    std::cout << NOW << " [DBG][V" << myId_ << "] LEADER_DB_EXCHANGE from lane "
              << (int)senderLaneIndex << " (V" << senderId << ") "
              << numEntries << " entries. Have "
              << receivedLeaderDBs_.size() << "/" << numLanes << " leader DBs." << std::endl;

    if ((int)receivedLeaderDBs_.size() >= numLanes)
        tryFormRaftFromLeaderDBs();
}

// Compute RAFT cluster membership from all received leader DBs and send invites.
// Membership = strict intersection of all leader DBs, with all lane leader senders
// force-included (so the cluster always has at least one vehicle per lane).
void RaftAppBase::tryFormRaftFromLeaderDBs()
{
    if (raftStarted_) return;

    int numLanes = (int)approachEdgeList_.size();

    // Debug: log which lanes are present and which are missing
    std::cout << NOW << " [DBG][V" << myId_ << "] TRY_FORM_RAFT: received leader DBs from lanes [";
    for (auto& kv : receivedLeaderDBs_) std::cout << kv.first << " ";
    std::cout << "]. Expected " << numLanes << " lanes." << std::endl;

    std::set<int> missingLanes;
    for (int i = 0; i < numLanes; i++) {
        if (!receivedLeaderDBs_.count(i)) missingLanes.insert(i);
    }
    if (!missingLanes.empty()) {
        std::cout << NOW << " [DBG][V" << myId_ << "] TRY_FORM_RAFT: missing leader DBs for lanes [";
        for (int l : missingLanes) std::cout << l << " ";
        std::cout << "] — proceeding with available DBs." << std::endl;
    }

    // Collect all lane leader sender vehicle IDs (force-included in cluster)
    std::set<int> laneLeaderIds;
    for (auto& kv : receivedLeaderSenderIds_) laneLeaderIds.insert(kv.second);
    laneLeaderIds.insert(myId_);  // include self

    // Strict intersection of all received leader DBs
    std::set<int> intersection;
    bool first = true;
    for (auto& kv : receivedLeaderDBs_) {
        std::set<int> thisSet;
        for (auto& e : kv.second) thisSet.insert(e.first);
        if (first) {
            intersection = thisSet;
            first = false;
        } else {
            std::set<int> tmp;
            for (int v : intersection)
                if (thisSet.count(v)) tmp.insert(v);
            intersection = tmp;
        }
    }

    // Force-include all lane leaders
    for (int lid : laneLeaderIds) intersection.insert(lid);

    if (intersection.empty()) {
        std::cout << NOW << " [WARN][V" << myId_ << "] TRY_FORM_RAFT: intersection is empty! Aborting." << std::endl;
        return;
    }

    std::cout << NOW << " [DBG][V" << myId_ << "] TRY_FORM_RAFT: RAFT cluster members ["
              << intersection.size() << "]: [";
    for (int v : intersection) std::cout << v << " ";
    std::cout << "]" << std::endl;

    // Only lane leaders send invites (all 4 send; recipient takes first, ignores duplicates)
    if (isLaneLeader_) {
        sendClusterJoinInvite(intersection);
    }
    // Non-leaders wait for a CLUSTER_JOIN_INVITE (handled in handleClusterJoinInvite).
    // If they are also in the intersection set they will receive an invite from a lane leader.
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
