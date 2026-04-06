// RaftCore.cc — RAFT protocol: election, log replication, serialization, and proposal submission
// Handles all willemt/raft library interactions. Does NOT contain the scheduling algorithm
// (see RaftDecision.cc) or follower-side handlers (see RaftCoordination.cc).

#include "raft/RaftAppBase.h"
#include "raft/RaftLogger.h"

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
// Increments failedElectionCount_ when term advances without this node winning.
// Activates fallback if too many consecutive failures while stopped.
void RaftAppBase::checkElectionFailures(raft_term_t currentTerm)
{
    if (currentTerm <= lastCheckedTerm_) return;
    electionRounds_++;
    if (!raft_is_leader(raftServer_)) {
        failedElectionCount_++;
        if (failedElectionCount_ >= maxFailedElections_
            && hasStoppedAtIntersection_ && !hasCommittedOrder_) {
            isFallbackMode_     = true;
            coordinationMethod_ = "fallback";
            handleFallback();
        }
    } else {
        failedElectionCount_ = 0;
    }
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

// ---- checkCoordinationTimeout -----------------------------------------------
// Cluster formed but coordination is stuck (STATUS_REQUEST responses lost, etc.).
// Skips if a follower already has a known leader (leader is still working).
void RaftAppBase::checkCoordinationTimeout(simtime_t now)
{
    // Follower with a known leader — leader is in charge, do not time out.
    if (!isLeader_ && raftServer_) {
        if (raft_get_current_leader(raftServer_) != -1) return;
    }

    simtime_t refTime;
    if (isLeader_ && timeElected_ > SIMTIME_ZERO) {
        refTime = timeElected_;
    } else {
        refTime = (timeStopped_ > timeClusterFormed_) ? timeStopped_ : timeClusterFormed_;
    }

    double extraPerVehicle = electionTimeoutBaseMs_ / 1000.0;
    double raftTimeout = (intersectionStopTimeMs_ + statusCollectionTimeoutMs_ +
                          electionTimeoutBaseMs_ * 4 + electionTimeoutJitterMs_ * 2 +
                          totalVehicles_ * extraPerVehicle * 1000.0 + 15000) / 1000.0;

    if ((now - refTime).dbl() > raftTimeout) {
        RAFT_LOG("RAFT TIMEOUT: " << (now - refTime).dbl() * 1000.0
                 << "ms since ref (threshold=" << raftTimeout * 1000.0 << "ms)");
        handleFallback();
    }
}

// ---- checkDiscoveryTimeout --------------------------------------------------
// Safety net: cluster never formed (discovery beacons never reached enough vehicles).
void RaftAppBase::checkDiscoveryTimeout(simtime_t now)
{
    double discTimeout = 15.0 + totalVehicles_ * 2.0;
    if ((now - timeStopped_).dbl() > discTimeout) {
        RAFT_LOG("DISCOVERY TIMEOUT (safety net): " << (now - timeStopped_).dbl() * 1000.0 << "ms");
        handleFallback();
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

    static int print_count = 0;
    if (print_count++ < 10) {
        RAFT_LOG("processRaftPeriodic delta=" << delta << " ms=" << msecElapsed);
    }

    // Tick the RAFT library — sends heartbeats, triggers elections.
    int old_state = raft_get_state(raftServer_);
    raft_periodic(raftServer_, msecElapsed);
    int new_state = raft_get_state(raftServer_);

    if (old_state != new_state) {
        RAFT_LOG("RAFT STATE " << old_state << " -> " << new_state);
        if (new_state == 2) RAFT_LOG("CANDIDATE — starting election");
    }

    checkElectionFailures(raft_get_current_term(raftServer_));
    if (isFallbackMode_) return;  // checkElectionFailures may have activated fallback

    checkLeadershipChange();

    if (hasStoppedAtIntersection_ && !hasCommittedOrder_ && !isFallbackMode_) {
        if (timeClusterFormed_ > SIMTIME_ZERO) {
            checkCoordinationTimeout(now);
        } else {
            checkDiscoveryTimeout(now);
        }
    }
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
    RAFT_LOG("sendRequestVote called for node " << raft_node_get_id(node));
    auto data = serializeRequestVote(msg);
    sendRaftToPeer(raft_node_get_id(node) - 1, /*RAFT_REQUEST_VOTE*/ 32, data);
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
    sendRaftToPeer(targetVehicleId, /*RAFT_APPEND_ENTRIES*/ 0x22, data);
    messagesSent_++;
    return 0;
}

int RaftAppBase::doLogOffer(raft_entry_t* entry, raft_index_t entry_idx)
{
    RAFT_LOG("doLogOffer entry #" << entry_idx << " (len=" << entry->data.len << ")");
    return (entry->data.len >= 1) ? 0 : -1;
}

int RaftAppBase::doLogGetNodeId(raft_entry_t* entry, raft_index_t entry_idx)
{
    if (!entry || entry->data.len < 1) return -1;
    uint8_t* data = static_cast<uint8_t*>(entry->data.buf);
    LogEntryType type = static_cast<LogEntryType>(data[0]);

    if (type == VEHICLE_LEFT && entry->data.len >= sizeof(VehicleLeftEntry) + 1) {
        VehicleLeftEntry* leftEntry = reinterpret_cast<VehicleLeftEntry*>(data + 1);
        return getNodeIdFromVehicleId(leftEntry->vehicleId);
    }
    return -1;
}

int RaftAppBase::doApplyLog(raft_entry_t* entry, raft_index_t entry_idx)
{
    RAFT_LOG("doApplyLog entry #" << entry_idx
             << " (lastApplied=" << lastAppliedIndex_
             << ", len=" << entry->data.len << ")");

    if (entry_idx <= lastAppliedIndex_) return 0;
    if (entry->data.len < 1)           return 0;

    uint8_t* data       = static_cast<uint8_t*>(entry->data.buf);
    uint8_t  entry_type = data[0];

    if (entry_type == PASS_ORDER) {
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

        if (proposedTimes_.count(entry_idx)) {
            double delta = (NOW - proposedTimes_[entry_idx]).dbl();
            totalRaftDecisionTimeSec_ += delta;
            proposedTimes_.erase(entry_idx);
        }

        applyCommittedPassOrder();
        // All cluster members broadcast the schedule immediately for redundancy.
        // In UDP, packet loss means a single broadcast may be dropped; multiple senders
        // give non-cluster vehicles more chances to receive it.
        // The leader will send a second broadcast once the QC is assembled (with QC embedded).
        // Receivers deduplicate via hasCommittedOrder_.
        sendPassOrderBroadcast();

        // Every cluster member signs [round || schedule] immediately upon commit.
        // Followers send their ECDSA signature directly to the leader (no request needed).
        // Leader collects signatures; once quorum reached it assembles the QC and sends
        // another PASS_ORDER_BROADCAST with the QC embedded.
        if (!qcAssembled_) {
            size_t tbsSize = sizeof(uint32_t) + sizeof(PassScheduleEntry);
            std::vector<uint8_t> tbs(tbsSize);
            uint32_t rn = static_cast<uint32_t>(roundNumber_);
            memcpy(tbs.data(),                    &rn,                sizeof(uint32_t));
            memcpy(tbs.data() + sizeof(uint32_t), &committedSchedule_, sizeof(PassScheduleEntry));

            if (isLeader_) {
                // Leader signs its own copy first.
                QCSigEntry mySig;
                memcpy(mySig.pubKey, myPubKey_, CRYPTO_PUBKEY_BYTES);
                mySig.sigLen = 0;
                CryptoAuth::instance().signBytes(myPrivKey_, tbs.data(), tbs.size(),
                                                 mySig.sig, mySig.sigLen);
                collectedQCSigs_[myId_] = mySig;
                std::cout << NOW << " [QC][V" << myId_ << "] Leader signed own QC copy (round="
                          << roundNumber_ << " clusterSize=" << activeVehicles_.size() << ")" << std::endl;
                tryAssembleQC();  // single-node cluster edge case
            } else if (raftServer_) {
                // Follower: sign and send directly to leader — no QC_SIGN_REQUEST needed.
                raft_node_id_t leaderNodeId = raft_get_current_leader(raftServer_);
                if (leaderNodeId > 0) {
                    int leaderVehicleId = static_cast<int>(leaderNodeId) - 1;
                    uint8_t sig[CRYPTO_SIG_MAX_BYTES];
                    uint8_t sigLen = 0;
                    CryptoAuth::instance().signBytes(myPrivKey_, tbs.data(), tbs.size(),
                                                     sig, sigLen);
                    std::vector<uint8_t> resp(sizeof(int) + CRYPTO_PUBKEY_BYTES + CRYPTO_SIG_MAX_BYTES + 1);
                    size_t off = 0;
                    memcpy(resp.data() + off, &myId_,    sizeof(int));           off += sizeof(int);
                    memcpy(resp.data() + off, myPubKey_, CRYPTO_PUBKEY_BYTES);   off += CRYPTO_PUBKEY_BYTES;
                    memcpy(resp.data() + off, sig,       CRYPTO_SIG_MAX_BYTES);  off += CRYPTO_SIG_MAX_BYTES;
                    resp[off] = sigLen;
                    sendQCSignResponse(leaderVehicleId, resp);
                    std::cout << NOW << " [QC][V" << myId_ << "] Signed and sent QC_SIGN_RESPONSE"
                              << " to leader V" << leaderVehicleId
                              << " (round=" << roundNumber_ << ")" << std::endl;
                }
            }
        }
        return 0;
    }

    if (entry_type == VEHICLE_LEFT) {
        VehicleLeftEntry* leftEntry = reinterpret_cast<VehicleLeftEntry*>(data + 1);
        RAFT_LOG("APPLYING VEHICLE_LEFT entry #" << entry_idx
                 << " - vehicle " << leftEntry->vehicleId
                 << " left batch " << leftEntry->batchId);
        logEntriesCommitted_++;
        lastAppliedIndex_ = entry_idx;

        if (proposedTimes_.count(entry_idx)) {
            double delta = (NOW - proposedTimes_[entry_idx]).dbl();
            totalRaftDecisionTimeSec_ += delta;
            proposedTimes_.erase(entry_idx);
        }

        applyVehicleLeftFromRaft(leftEntry->vehicleId, leftEntry->batchId);
        return 0;
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

// ============ STATUS REQUEST (leader-side send) ============

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

    timeStatusRequestSent_ = NOW;
    waitingForStatus_     = true;
    statusResponseCount_  = 0;
    collectedLeaderDBs_.clear();

    // Leader adds its own vehicleDB_ immediately (broadcasts don't loop back to sender)
    vehicleDB_[myId_] = updateMyProposal();
    collectedLeaderDBs_[myId_] = vehicleDB_;
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
    // This ensures each lane leader's fresh self-reported position (dist, laneIndex, etc.)
    // always wins over stale beacon data that other vehicles may carry for it.
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
            RAFT_LOG_LEADER("lane leader " << vid << " DB response lost — using default proposal");
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
        proposedTimes_[response.idx] = NOW;
        RAFT_LOG_LEADER("submitted PASS_SCHEDULE entry #" << response.idx);

        raft_index_t idx = response.idx;
        scheduleOneshotMs(100, [this, idx, schedule]() {
            if (raftServer_ && raft_get_commit_idx(raftServer_) >= idx && !hasCommittedOrder_) {
                RAFT_LOG_LEADER("APPLYING committed PASS_SCHEDULE #" << idx);
                if (proposedTimes_.count(idx)) {
                    double delta = (NOW - proposedTimes_[idx]).dbl();
                    totalRaftDecisionTimeSec_ += delta;
                    proposedTimes_.erase(idx);
                }
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

void RaftAppBase::applyVehicleLeftFromRaft(int vehicleId, int batchId)
{
    RAFT_LOG("RAFT-confirmed: vehicle " << vehicleId
             << " left batch " << batchId);
    markRaftNodeInactive(vehicleId);
    activeVehicles_.erase(vehicleId);
    vehiclesLeftInBatch_.insert(vehicleId);
    checkBatchAdvance();
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
    RAFT_LOG("BECAME LEADER (term=" << raft_get_current_term(raftServer_) << ")");
    wasElectedLeader_     = true;
    timeElected_          = NOW;
    failedElectionCount_  = 0;

    // Only propose status/pass order after 5s stopped (aggressive merge window)
    if (!hasStoppedAtIntersection_ || hasCommittedOrder_) return;

    double waitedMs       = (NOW - timeStopped_).dbl() * 1000.0;
    double delayMs       = intersectionStopTimeMs_ - waitedMs;

    if (delayMs <= 0) {
        sendStatusRequest();
        scheduleOneshotMs(statusCollectionTimeoutMs_, [this]() {
            if (isLeader_ && !hasCommittedOrder_ && !hasPassedIntersection_)
                collectStatusAndDecide();
        });
    } else {
        scheduleOneshotMs(delayMs, [this]() {
            if (isLeader_ && !hasCommittedOrder_ && !hasPassedIntersection_
                && hasStoppedAtIntersection_) {
                sendStatusRequest();
                scheduleOneshotMs(statusCollectionTimeoutMs_, [this]() {
                    if (isLeader_ && !hasCommittedOrder_ && !hasPassedIntersection_)
                        collectStatusAndDecide();
                });
            }
        });
    }
}

void RaftAppBase::onLostLeadership()
{
    RAFT_LOG("LOST leadership");
    waitingForStatus_ = false;
}

// ============ PASS ORDER BROADCAST (to non-cluster vehicles) ============

// Called by the RAFT leader once the QC is assembled.
// Payload: [PassScheduleEntry][QuorumCertificate] — schedule + crypto proof in one message.
// Non-cluster (queued) vehicles receive this and apply both the schedule and store the QC.
void RaftAppBase::sendPassOrderBroadcast()
{
    if (!hasCommittedOrder_) return;

    // Embed QC in payload so non-cluster vehicles get schedule + proof in one message.
    std::vector<uint8_t> data(sizeof(PassScheduleEntry) + sizeof(QuorumCertificate));
    memcpy(data.data(),                          &committedSchedule_, sizeof(PassScheduleEntry));
    memcpy(data.data() + sizeof(PassScheduleEntry), &prevRoundQC_,    sizeof(QuorumCertificate));
    sendRaftBroadcast(/*COORD_PASS_ORDER_BROADCAST*/ 0x35, data);
    messagesSent_++;

    std::cout << simTime() << " [DBG][V" << myId_ << "] PASS_ORDER_BROADCAST sent (with QC): "
              << committedSchedule_.numBatches << " batches, QC round=" << prevRoundQC_.round
              << " numSigs=" << prevRoundQC_.numSigs << std::endl;
    RAFT_LOG("PASS_ORDER_BROADCAST sent (" << committedSchedule_.numBatches << " batches + QC)");
}
