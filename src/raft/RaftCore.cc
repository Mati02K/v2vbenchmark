// RaftCore.cc — Post-stop RAFT protocol: election, log replication, status
//               collection, pass-order proposal, and QC signing/assembly.
//
// Flow after formCluster() is called:
//   1. processRaftPeriodic() ticks RAFT every 20ms — triggers election
//   2. onBecameLeader() fires — sends STATUS_REQUEST to all cluster members
//   3. Followers respond via handleStatusRequest() → sendDbResponse()
//   4. Leader collects DBs in handleDbResponse() → calls proposePassOrder()
//   5. proposePassOrder() calls computePassOrder() (RaftDecision.cc) → submits RAFT entry
//   6. doApplyLog() commits PASS_ORDER — each member signs it → tryAssembleQC()
//   7. tryAssembleQC() assembles QC → calls sendPassOrderBroadcast() (RaftCoordination.cc)

#include "raft/RaftAppBase.h"
#include "raft/RaftTypes_m.h"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <functional>

extern "C" {
#include "../../lib/raft/raft.h"
}

#define NOW (simTime())

// ============ RAFT PERIODIC ============

// ---- checkElectionFailures --------------------------------------------------
// Tracks election rounds for metrics.
void RaftAppBase::checkElectionFailures(raft_term_t currentTerm)
{
    if (currentTerm <= lastCheckedTerm_) return;
    electionRounds_++;
    lastCheckedTerm_ = currentTerm;
}

// ---- checkLeadershipChange --------------------------------------------------
// Detects FOLLOWER→LEADER and LEADER→FOLLOWER transitions this tick.
void RaftAppBase::checkLeadershipChange()
{
    bool wasLeader = isLeader_;
    isLeader_ = (raft_is_leader(raftServer_) == 1);
    if (isLeader_ && !wasLeader) {
        onBecameLeader();
    } else if (!isLeader_ && wasLeader) {
        onLostLeadership();
    }
}


// ---- processRaftPeriodic ----------------------------------------------------
// Called every 20ms. Ticks the RAFT library and runs watchdog checks.
void RaftAppBase::processRaftPeriodic()
{
    if (!raftServer_ || isFallbackMode_) return;

    simtime_t now = NOW;
    if (lastRaftPeriodicRun_ == SIMTIME_ZERO) lastRaftPeriodicRun_ = now;
    simtime_t delta = now - lastRaftPeriodicRun_;
    lastRaftPeriodicRun_ = now;

    int msecElapsed = static_cast<int>(delta.dbl() * 1000.0);
    if (msecElapsed <= 0) return;

    // Tick the RAFT library — sends heartbeats, triggers elections.
    int old_state = raft_get_state(raftServer_);
    raft_periodic(raftServer_, msecElapsed);
    int new_state = raft_get_state(raftServer_);

    if (old_state != new_state) {
        std::cout << simTime() << " [DBG][V" << myId_ << "] RAFT STATE " << old_state << " -> " << new_state << std::endl;
        if (new_state == 2) {
            std::cout << simTime() << " [DBG][V" << myId_ << "] CANDIDATE — starting election" << std::endl;
        }
    }

    checkElectionFailures(raft_get_current_term(raftServer_));
    checkLeadershipChange();
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

int RaftAppBase::logGetNodeId(raft_server_t*, void* user_data,
                              raft_entry_t* entry, raft_index_t entry_idx)
{
    return static_cast<RaftAppBase*>(user_data)->doLogGetNodeId(entry, entry_idx);
}

// ============ RAFT LOG CALLBACKS ============

int RaftAppBase::doSendRequestVote(raft_node_t* node, msg_requestvote_t* msg)
{
    auto data = serializeRequestVote(msg);
    sendRaftToPeer(raft_node_get_id(node) - 1, benchmark::RAFT_REQUEST_VOTE, data);
    return 0;
}

int RaftAppBase::doSendAppendEntries(raft_node_t* node, msg_appendentries_t* msg)
{
    if (!node) return -1;
    void* udata = raft_node_get_udata(node);
    int targetVehicleId = static_cast<int>(reinterpret_cast<intptr_t>(udata));
    if (activeVehicles_.find(targetVehicleId) == activeVehicles_.end()) return 0;

    std::cout << simTime() << " [DBG][V" << myId_ << "] SENDING AppendEntries to V" << targetVehicleId
              << " n_entries=" << msg->n_entries << " leader_commit=" << msg->leader_commit << std::endl;

    auto data = serializeAppendEntries(msg);
    sendRaftToPeer(targetVehicleId, benchmark::RAFT_APPEND_ENTRIES, data);
    return 0;
}

int RaftAppBase::doLogOffer(raft_entry_t* entry, raft_index_t entry_idx)
{
    std::cout << simTime() << " [DBG][V" << myId_ << "] doLogOffer entry #" << entry_idx << " len=" << entry->data.len << std::endl;
    return (entry->data.len >= 1) ? 0 : -1;
}

int RaftAppBase::doLogGetNodeId(raft_entry_t* entry, raft_index_t entry_idx)
{
    return -1;
}

// Once quorum is reached, apply the committed pass schedule.
int RaftAppBase::doApplyLog(raft_entry_t* entry, raft_index_t entry_idx)
{
    std::cout << simTime() << " [DBG][V" << myId_ << "] doApplyLog entry #" << entry_idx
              << " lastApplied=" << lastAppliedIndex_ << " len=" << entry->data.len << std::endl;

    if (entry_idx <= lastAppliedIndex_) return 0;
    if (entry->data.len < sizeof(PassScheduleEntry)) return 0;

    PassScheduleEntry* schedule = reinterpret_cast<PassScheduleEntry*>(entry->data.buf);
    std::cout << simTime() << " [DBG][V" << myId_ << "] APPLYING pass schedule entry #" << entry_idx
              << " (" << schedule->numBatches << " batches)" << std::endl;

    memcpy(&committedSchedule_, schedule, sizeof(PassScheduleEntry));
    hasCommittedOrder_ = true;
    if (hasStoppedAtIntersection_ && timeStopped_ > SIMTIME_ZERO) {
        timeOrderCommitted_ = NOW;
    }
    logEntriesCommitted_++;
    lastAppliedIndex_ = entry_idx;

    if (proposedTimes_.count(entry_idx)) {
        double delta = (NOW - proposedTimes_[entry_idx]).dbl();
        totalRaftDecisionTimeSec_ += delta;
        proposedTimes_.erase(entry_idx);
    }

    sendPassOrderBroadcast();
    applyCommittedPassOrder();
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

// ============ LEADERSHIP TRANSITIONS ============

void RaftAppBase::onBecameLeader()
{
    std::cout << simTime() << " [DBG][V" << myId_ << "] BECAME LEADER term="
              << raft_get_current_term(raftServer_)
              << " stopped=" << hasStoppedAtIntersection_
              << " committed=" << hasCommittedOrder_
              << " activeVehicles=[";
    for (int v : activeVehicles_)
    {
        std::cout << v << " ";
    }
    std::cout << "]" << std::endl;
    wasElectedLeader_     = true;
    timeElected_          = NOW;

    if (hasCommittedOrder_) return;

    sendStatusRequest();
    scheduleOneshotMs(statusCollectionTimeoutMs_, [this]() {
        if (isLeader_ && !hasCommittedOrder_ && !hasPassedIntersection_)
            collectStatusAndDecide();
    });
}

void RaftAppBase::onLostLeadership()
{
    std::cout << simTime() << " [DBG][V" << myId_ << "] LOST leadership" << std::endl;
    waitingForStatus_ = false;
}

// ============ STATUS REQUEST (leader-side send) ============

void RaftAppBase::sendStatusRequest()
{
    std::cout << simTime() << " [DBG][V" << myId_ << "] SEND_STATUS_REQUEST to "
              << activeVehicles_.size() << " active vehicles, timeout="
              << statusCollectionTimeoutMs_ << "ms" << std::endl;

    // Payload carries senderId — must be non-empty for INET UDP compatibility
    std::vector<uint8_t> data(sizeof(int));
    memcpy(data.data(), &myId_, sizeof(int));
    sendRaftBroadcast(benchmark::COORD_STATUS_REQUEST, data);

    timeStatusRequestSent_ = NOW;
    waitingForStatus_     = true;
    statusResponseCount_  = 0;
    collectedLeaderDBs_.clear();

    // Leader adds its own vehicleDB_ immediately (broadcasts don't loop back to sender)
    vehicleDB_[myId_] = updateMyProposal();
    collectedLeaderDBs_[myId_] = vehicleDB_;
}

// ============ STATUS REQUEST / RESPONSE (follower-side) ============

void RaftAppBase::handleStatusRequest(int fromLeader)
{
    if (hasPassedIntersection_) return;
    // Only RAFT cluster members (lane leaders) respond to STATUS_REQUEST.
    // Queued vehicles are passive and wait for COORD_PASS_ORDER_BROADCAST instead.
    if (!raftServer_) {
        std::cout << simTime() << " [DBG][V" << myId_ << "] STATUS_REQUEST from V" << fromLeader
                  << " ignored — not a RAFT cluster member (queued vehicle)" << std::endl;
        return;
    }
    sendDbResponse(fromLeader);
}

// Lane leader sends its full vehicleDB_ to the RAFT leader.
// Payload: [senderVehicleId:4B][numEntries:2B][VehicleProposal * numEntries]
void RaftAppBase::sendDbResponse(int toLeader)
{
    // Refresh own entry in vehicleDB_ before sending
    vehicleDB_[myId_] = updateMyProposal();

    uint16_t numEntries = static_cast<uint16_t>(vehicleDB_.size());
    std::vector<uint8_t> data(sizeof(int) + sizeof(uint16_t) + numEntries * sizeof(VehicleProposal));
    size_t off = 0;
    memcpy(data.data() + off, &myId_, sizeof(int));
    off += sizeof(int);
    memcpy(data.data() + off, &numEntries, sizeof(uint16_t));
    off += sizeof(uint16_t);
    for (auto& kv : vehicleDB_) {
        memcpy(data.data() + off, &kv.second, sizeof(VehicleProposal));
        off += sizeof(VehicleProposal);
    }

    sendRaftToPeer(toLeader, benchmark::COORD_STATUS_RESPONSE, data);

    std::cout << simTime() << " [DBG][V" << myId_ << "] DB_RESPONSE sent to V" << toLeader
              << " numEntries=" << numEntries << std::endl;
}

// Leader receives a lane leader's full vehicleDB_.
// When all lane leaders have responded, merge all DBs and call proposePassOrder().
void RaftAppBase::handleDbResponse(const std::vector<uint8_t>& data, int senderId)
{
    if (!isLeader_ || !waitingForStatus_) return;
    if (data.size() < sizeof(int) + sizeof(uint16_t)) return;

    size_t off = 0;
    int claimedId;
    memcpy(&claimedId, data.data() + off, sizeof(int));         off += sizeof(int);
    uint16_t numEntries;
    memcpy(&numEntries, data.data() + off, sizeof(uint16_t));   off += sizeof(uint16_t);

    if (claimedId != senderId) {
        std::cout << simTime() << " [WARN][V" << myId_ << "] DB_RESPONSE: id mismatch"
                  << " claimed=" << claimedId << " sender=" << senderId << " — ignored" << std::endl;
        return;
    }
    if (data.size() < off + numEntries * sizeof(VehicleProposal)) return;

    std::map<int, VehicleProposal> senderDb;
    for (int i = 0; i < numEntries; i++) {
        VehicleProposal prop;
        memcpy(&prop, data.data() + off, sizeof(VehicleProposal));
        off += sizeof(VehicleProposal);
        if (prop.vehicleId >= 0 && prop.vehicleId < totalVehicles_) {
            senderDb[prop.vehicleId] = prop;
        }
    }
    collectedLeaderDBs_[senderId] = senderDb;
    statusResponseCount_++;

    int expectedResponses = std::max(1, (int)activeVehicles_.size() - 1);
    std::cout << simTime() << " [DBG][V" << myId_ << "] DB_RESPONSE from V" << senderId
              << " entries=" << numEntries
              << " collected=" << statusResponseCount_ << "/" << expectedResponses << std::endl;

    if (statusResponseCount_ >= expectedResponses) {
        waitingForStatus_ = false;
        if (timeStatusRequestSent_ > SIMTIME_ZERO) {
            statusCollectionTimeMs_ += (NOW - timeStatusRequestSent_).dbl() * 1000.0;
        }
        std::cout << simTime() << " [DBG][V" << myId_ << "] ALL DB RESPONSES COLLECTED -> proposePassOrder()" << std::endl;
        proposePassOrder();
    }
}

void RaftAppBase::collectStatusAndDecide()
{
    if (!isLeader_ || hasPassedIntersection_ || hasCommittedOrder_) return;
    waitingForStatus_ = false;
    if (timeStatusRequestSent_ > SIMTIME_ZERO) {
        statusCollectionTimeMs_ += (NOW - timeStatusRequestSent_).dbl() * 1000.0;
    }

    std::cout << simTime() << " [DBG][V" << myId_ << "] status collection timeout — DBs from " << collectedLeaderDBs_.size() << " leaders" << std::endl;
    proposePassOrder();
}

// ============ PASS ORDER PROPOSAL (RAFT wrapper) ============
// The scheduling algorithm itself is in RaftDecision.cc::computePassOrder().

void RaftAppBase::proposePassOrder()
{
    if (!raftServer_ || !isLeader_ || hasCommittedOrder_ || passOrderProposed_) return;
    passOrderProposed_ = true;

    // Merge all lane leaders' DBs into allProposals.
    // collectedLeaderDBs_ contains: leader's own DB (set in sendStatusRequest) +
    // each follower's DB received via COORD_DB_RESPONSE.
    std::map<int, VehicleProposal> allProposals;
    std::set<int> allVehicleIds;

    // Two-pass merge: first collect all data, then overwrite with self-reports.
    for (auto& leaderKv : collectedLeaderDBs_) {
        for (auto& propKv : leaderKv.second) {
            int vid = propKv.first;
            if (allProposals.count(vid) == 0) {
                allProposals[vid] = propKv.second;
            }
            allVehicleIds.insert(vid);
        }
    }
    // Second pass: self-reports always win (overwrite stale beacon data from other DBs)
    for (auto& leaderKv : collectedLeaderDBs_) {
        int leaderId = leaderKv.first;
        if (leaderKv.second.count(leaderId)) {
            allProposals[leaderId] = leaderKv.second.at(leaderId);
        }
    }

    // Add default proposals for any lane leader whose DB response was lost (packet loss)
    int vehiclesPerSide = std::max(totalVehicles_ / 4, 1);
    for (int vid : activeVehicles_) {
        if (allVehicleIds.count(vid) == 0) {
            VehicleProposal dflt;
            memset(&dflt, 0, sizeof(dflt));
            dflt.vehicleId          = vid;
            dflt.laneIndex          = (vid / vehiclesPerSide) % 4;
            dflt.intendedTurn       = 0;
            dflt.isFirstInLane      = true;
            dflt.blockedByVehicleId = -1;
            dflt.waitingTimeMs      = 99999.0;
            dflt.distanceToJunction = 0.0;
            allProposals[vid]       = dflt;
            allVehicleIds.insert(vid);
            std::cout << simTime() << " [DBG][V" << myId_ << "] lane leader V" << vid << " DB response lost — using default proposal" << std::endl;
        }
    }

    std::cout << simTime() << " [DBG][V" << myId_ << "] proposePassOrder: merged DBs from "
              << collectedLeaderDBs_.size() << " leaders → " << allVehicleIds.size()
              << " total vehicles" << std::endl;

    // ---- Multi-round: skip vehicles already scheduled in previous rounds ----
    if (!scheduledVehicles_.empty()) {
        for (int vid : scheduledVehicles_) {
            allProposals.erase(vid);
            allVehicleIds.erase(vid);
        }
        std::cout << simTime() << " [ROUND][V" << myId_ << "] proposePassOrder: skipping "
                  << scheduledVehicles_.size() << " vehicles from previous rounds. "
                  << allVehicleIds.size() << " remain." << std::endl;
    }

    if (allVehicleIds.empty()) {
        std::cout << simTime() << " [ROUND][V" << myId_
                  << "] proposePassOrder: all vehicles already scheduled — nothing to do." << std::endl;
        return;
    }

    std::cout << simTime() << " [DBG][V" << myId_ << "] proposePassOrder: scheduling "
              << allVehicleIds.size() << " vehicles (round=" << roundNumber_
              << " activeVehicles_=" << activeVehicles_.size()
              << " vehicleDB_=" << vehicleDB_.size() << ")" << std::endl;

    // Call the decision algorithm (defined in RaftDecision.cc)
    PassScheduleEntry schedule = computePassOrder(allProposals, allVehicleIds);

    if (schedule.numBatches == 0) return;

    // Log schedule
    std::cout << simTime() << " [DBG][V" << myId_ << "] PASS_SCHEDULE computed (" << schedule.numBatches << " batches)" << std::endl;
    for (int b = 0; b < schedule.numBatches; b++) {
        std::cout << "  Batch " << b << ": [";
        for (int v = 0; v < schedule.batches[b].numVehicles; v++) {
            std::cout << schedule.batches[b].vehicleIds[v];
            if (v < schedule.batches[b].numVehicles - 1) std::cout << ",";
        }
        std::cout << "]" << std::endl;
    }

    // Store schedule — RAFT submission happens only after quorum of signatures collected.
    memcpy(&pendingSchedule_, &schedule, sizeof(PassScheduleEntry));

    // Broadcast QC_SIGN_REQUEST to all cluster members so they sign [round || schedule].
    // Once quorum is reached in tryAssembleQC(), raft_recv_entry() is called.
    qcRetryCount_ = 0;
    collectedQCSigs_.clear();
    sendQCSignRequest();
}

// ============ QUORUM CERTIFICATE ============

// Leader broadcasts [round || pendingSchedule_] to all cluster members asking them to sign.
// Leader also signs its own copy immediately.
void RaftAppBase::sendQCSignRequest()
{
    if (!isLeader_ || qcAssembled_) return;

    // Payload: [uint32_t round][PassScheduleEntry schedule]
    size_t payloadSize = sizeof(uint32_t) + sizeof(PassScheduleEntry);
    std::vector<uint8_t> data(payloadSize);
    uint32_t rn = static_cast<uint32_t>(roundNumber_);
    memcpy(data.data(),                    &rn,               sizeof(uint32_t));
    memcpy(data.data() + sizeof(uint32_t), &pendingSchedule_, sizeof(PassScheduleEntry));

    sendRaftBroadcast(benchmark::QC_SIGN_REQUEST, data);
    std::cout << NOW << " [QC][V" << myId_ << "] QC_SIGN_REQUEST broadcast (round=" << roundNumber_
              << " clusterSize=" << activeVehicles_.size() << ")" << std::endl;

    // Leader signs its own copy immediately.
    uint8_t sig[CRYPTO_SIG_MAX_BYTES];
    uint8_t sigLen = 0;
    CryptoAuth::instance().signBytes(myPrivKey_, data.data(), data.size(), sig, sigLen);
    QCSigEntry mySig;
    memcpy(mySig.pubKey, myPubKey_, CRYPTO_PUBKEY_BYTES);
    memcpy(mySig.sig,    sig,       CRYPTO_SIG_MAX_BYTES);
    mySig.sigLen = sigLen;
    collectedQCSigs_[myId_] = mySig;
    std::cout << NOW << " [QC][V" << myId_ << "] Leader signed own copy" << std::endl;
    tryAssembleQC();  // in case single-node cluster

    // Schedule retry in case QC_SIGN_RESPONSE packets are lost
    if (!qcAssembled_ && qcRetryCount_ < maxQcRetries_) {
        scheduleOneshotMs((double)qcSignTimeoutMs_, [this]() {
            if (!qcAssembled_ && isLeader_ && qcRetryCount_ < maxQcRetries_) {
                qcRetryCount_++;
                std::cout << NOW << " [QC][V" << myId_ << "] QC_SIGN_REQUEST retry "
                          << qcRetryCount_ << "/" << maxQcRetries_
                          << " (have " << collectedQCSigs_.size() << " sigs)" << std::endl;
                sendQCSignRequest();
            }
        });
    }
}

// Follower receives QC_SIGN_REQUEST: sign [round || schedule] and return signature to leader.
void RaftAppBase::handleQCSignRequest(const std::vector<uint8_t>& data)
{
    if (isLeader_ || qcAssembled_) return;  // leader handles its own sign in sendQCSignRequest
    if (data.size() < sizeof(uint32_t) + sizeof(PassScheduleEntry)) return;
    if (!raftServer_) return;

    raft_node_id_t leaderNodeId = raft_get_current_leader(raftServer_);
    if (leaderNodeId <= 0) return;
    int leaderVehicleId = static_cast<int>(leaderNodeId) - 1;

    uint8_t sig[CRYPTO_SIG_MAX_BYTES];
    uint8_t sigLen = 0;
    CryptoAuth::instance().signBytes(myPrivKey_, data.data(), data.size(), sig, sigLen);

    std::vector<uint8_t> resp(sizeof(int) + CRYPTO_PUBKEY_BYTES + CRYPTO_SIG_MAX_BYTES + 1);
    size_t off = 0;
    memcpy(resp.data() + off, &myId_,    sizeof(int));          off += sizeof(int);
    memcpy(resp.data() + off, myPubKey_, CRYPTO_PUBKEY_BYTES);  off += CRYPTO_PUBKEY_BYTES;
    memcpy(resp.data() + off, sig,       CRYPTO_SIG_MAX_BYTES); off += CRYPTO_SIG_MAX_BYTES;
    resp[off] = sigLen;

    sendQCSignResponse(leaderVehicleId, resp);
    std::cout << NOW << " [QC][V" << myId_ << "] Signed QC_SIGN_REQUEST and sent response to V"
              << leaderVehicleId << " (round=" << roundNumber_ << ")" << std::endl;
}

void RaftAppBase::sendQCSignResponse(int toLeader, const std::vector<uint8_t>& respData)
{
    sendRaftToPeer(toLeader, benchmark::QC_SIGN_RESPONSE, respData);
}

void RaftAppBase::handleQCSignResponse(const std::vector<uint8_t>& data, int senderId)
{
    if (!isLeader_ || qcAssembled_) return;
    if (data.size() < sizeof(int) + CRYPTO_PUBKEY_BYTES + CRYPTO_SIG_MAX_BYTES + 1) return;

    size_t off = 0;
    int vid;
    memcpy(&vid, data.data() + off, sizeof(int)); off += sizeof(int);

    if (vid != senderId) {
        std::cout << NOW << " [QC][V" << myId_ << "] QC_SIGN_RESPONSE: vehicleId mismatch"
                  << " (claimed " << vid << " from packet sender " << senderId << ") — ignored" << std::endl;
        return;
    }
    if (!activeVehicles_.count(vid)) return;  // not a cluster member

    QCSigEntry entry;
    memcpy(entry.pubKey, data.data() + off, CRYPTO_PUBKEY_BYTES); off += CRYPTO_PUBKEY_BYTES;
    memcpy(entry.sig,    data.data() + off, CRYPTO_SIG_MAX_BYTES); off += CRYPTO_SIG_MAX_BYTES;
    entry.sigLen = data[off];

    collectedQCSigs_[vid] = entry;

    int required = static_cast<int>(activeVehicles_.size()) / 2 + 1;
    std::cout << NOW << " [QC][V" << myId_ << "] QC_SIGN_RESPONSE from V" << vid
              << " (" << collectedQCSigs_.size() << "/" << required << " sigs)" << std::endl;

    tryAssembleQC();
}

void RaftAppBase::tryAssembleQC()
{
    if (!isLeader_ || qcAssembled_) return;
    int required = static_cast<int>(activeVehicles_.size()) / 2 + 1;
    if (static_cast<int>(collectedQCSigs_.size()) < required) return;

    // Quorum reached — assemble the QC.
    qcAssembled_ = true;

    QuorumCertificate qc;
    memset(&qc, 0, sizeof(qc));
    qc.valid  = true;
    qc.round  = static_cast<uint32_t>(roundNumber_);
    memcpy(&qc.schedule, &committedSchedule_, sizeof(PassScheduleEntry));
    qc.numSigs = 0;
    for (auto& kv : collectedQCSigs_) {
        if (qc.numSigs >= QC_MAX_MEMBERS) break;
        QCSig& s    = qc.sigs[qc.numSigs];
        s.vehicleId = kv.first;
        memcpy(s.pubKey, kv.second.pubKey, CRYPTO_PUBKEY_BYTES);
        memcpy(s.sig,    kv.second.sig,    CRYPTO_SIG_MAX_BYTES);
        s.sigLen    = kv.second.sigLen;
        qc.numSigs++;
    }

    // Store QC locally — embedded in PASS_ORDER_BROADCAST after RAFT commits.
    prevRoundQC_    = qc;
    hasPrevRoundQC_ = true;
    for (int b = 0; b < qc.schedule.numBatches; b++)
        for (int v = 0; v < qc.schedule.batches[b].numVehicles; v++)
            scheduledVehicles_.insert(qc.schedule.batches[b].vehicleIds[v]);

    std::cout << NOW << " [QC][V" << myId_ << "] QC assembled: round=" << qc.round
              << " numSigs=" << qc.numSigs
              << " — submitting pass schedule to RAFT log" << std::endl;

    // Submit pass schedule to RAFT — quorum of signatures already collected.
    // doApplyLog() will call sendPassOrderBroadcast() and applyCommittedPassOrder().
    size_t entrySize = sizeof(PassScheduleEntry);
    uint8_t* entryData = new uint8_t[entrySize];
    memcpy(entryData, &pendingSchedule_, sizeof(PassScheduleEntry));

    raft_entry_t entry;
    memset(&entry, 0, sizeof(entry));
    entry.term     = raft_get_current_term(raftServer_);
    entry.type     = RAFT_LOGTYPE_NORMAL;
    entry.data.buf = entryData;
    entry.data.len = entrySize;

    msg_entry_response_t response;
    if (raft_recv_entry(raftServer_, &entry, &response) == 0) {
        logEntriesProposed_++;
        proposedTimes_[response.idx] = NOW;
        std::cout << simTime() << " [DBG][V" << myId_ << "] submitted PASS_SCHEDULE entry #" << response.idx << std::endl;
    } else {
        std::cerr << "Vehicle " << myId_ << " FAILED to submit PASS_SCHEDULE to RAFT" << std::endl;
        delete[] entryData;
    }
}

bool RaftAppBase::verifyQC(const QuorumCertificate& qc) const
{
    if (!qc.valid || qc.numSigs == 0) return false;

    // Build the TBS buffer [uint32_t round || PassScheduleEntry] that signers signed.
    size_t tbsSize = sizeof(uint32_t) + sizeof(PassScheduleEntry);
    std::vector<uint8_t> tbs(tbsSize);
    memcpy(tbs.data(),                    &qc.round,    sizeof(uint32_t));
    memcpy(tbs.data() + sizeof(uint32_t), &qc.schedule, sizeof(PassScheduleEntry));

    int validSigs = 0;
    for (int i = 0; i < qc.numSigs; i++) {
        bool validSign = CryptoAuth::instance().verifyBytes(
            qc.sigs[i].pubKey, tbs.data(), tbs.size(),
            qc.sigs[i].sig, qc.sigs[i].sigLen);
        if (validSign)
        {
            validSigs++;
        }
        else
        {
            std::cout << "[QC][V" << myId_ << "] verifyQC: sig " << i
                      << " from V" << qc.sigs[i].vehicleId << " INVALID" << std::endl;
        }
    }
    return validSigs >= qc.numSigs;  // all claimed sigs must verify
}
