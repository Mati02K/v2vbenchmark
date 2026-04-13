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

// Once quorum is reached and entry is committed, this callback applies the log entry to the state machine (pass order, vehicle left).
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

        // QC was already assembled before this commit — start movement and notify queued vehicles.
        // All cluster members broadcast so queued vehicles get the schedule even if one packet drops.
        applyCommittedPassOrder();
        sendPassOrderBroadcast();
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

    // Only propose status/pass order after stopped at intersection
    if (!hasStoppedAtIntersection_ || hasCommittedOrder_) return;

    double waitedMs = (NOW - timeStopped_).dbl() * 1000.0;
    double delayMs  = intersectionStopTimeMs_ - waitedMs;

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

    sendRaftToPeer(toLeader, /*COORD_DB_RESPONSE*/ 0x31, data);
    messagesSent_++;

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

    RAFT_LOG_LEADER("status collection timeout — collected DBs from " << collectedLeaderDBs_.size() << " leaders");
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
    RAFT_LOG_LEADER("PASS_SCHEDULE computed (" << schedule.numBatches << " batches) — starting QC signing before RAFT submission:");
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
    sendQCSignRequest();
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

    sendRaftBroadcast(/*QC_SIGN_REQUEST*/ 0x36, data);
    messagesSent_++;
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
    sendRaftToPeer(toLeader, /*QC_SIGN_RESPONSE*/ 0x37, respData);
    messagesSent_++;
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
              << " — submitting PASS_ORDER to RAFT log" << std::endl;

    // NOW submit to RAFT — quorum of signatures already collected.
    // doApplyLog() will call applyCommittedPassOrder() (start movement) and
    // sendPassOrderBroadcast() (deliver QC + schedule to queued vehicles).
    size_t entrySize = 1 + sizeof(PassScheduleEntry);
    uint8_t* entryData = new uint8_t[entrySize];
    entryData[0] = static_cast<uint8_t>(PASS_ORDER);
    memcpy(entryData + 1, &pendingSchedule_, sizeof(PassScheduleEntry));

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
        RAFT_LOG_LEADER("submitted PASS_SCHEDULE entry #" << response.idx << " (QC pre-verified)");
    } else {
        RAFT_LOG_LEADER("FAILED to submit PASS_SCHEDULE to RAFT");
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
