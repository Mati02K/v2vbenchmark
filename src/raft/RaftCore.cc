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
#include "../../third_party/raft/raft.h"
}

#define NOW (simTime())

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
            double raftTimeout = (discoveryWaitMs_ + clusterFormationDelayMs_ + statusCollectionTimeoutMs_ +
                                  electionTimeoutBaseMs_ * 4 + electionTimeoutJitterMs_ * 2 +
                                  totalVehicles_ * extraPerVehicle * 1000.0 +
                                  15000) / 1000.0;
            if (raftWait.dbl() > raftTimeout) {
                RAFT_LOG("RAFT TIMEOUT: " << raftWait.dbl()*1000.0
                         << "ms since ref (threshold=" << raftTimeout*1000.0 << "ms)");
                handleFallback();
            }
        } else {
            // Pre-cluster: safety net (200 ms trigger should handle this)
            simtime_t discWait    = now - timeStopped_;
            double    discTimeout = 15.0 + totalVehicles_ * 2.0;
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

    if (entry_type == STATUS_REPORT) {
        StatusReportEntry* report = reinterpret_cast<StatusReportEntry*>(data + 1);
        RAFT_LOG("APPLYING STATUS_REPORT entry #" << entry_idx
                 << " (" << report->numVehicles << " vehicles)");

        committedStatuses_.clear();
        for (int i = 0; i < report->numVehicles; i++) {
            committedStatuses_[report->statuses[i].vehicleId] = report->statuses[i];
        }
        logEntriesCommitted_++;
        lastAppliedIndex_ = entry_idx;

        if (proposedTimes_.count(entry_idx)) {
            double delta = (NOW - proposedTimes_[entry_idx]).dbl();
            totalRaftDecisionTimeSec_ += delta;
            proposedTimes_.erase(entry_idx);
        }

        if (isLeader_ && !hasPassedIntersection_ && !hasCommittedOrder_) {
            scheduleOneshotMs(50, [this]() { proposePassOrder(); });
        }
        return 0;
    }

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

        executePassOrder();
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

    if (entry_type == PASS_COMMAND) {
        PassCommandEntry* cmd = reinterpret_cast<PassCommandEntry*>(data + 1);
        RAFT_LOG("APPLYING PASS_COMMAND entry #" << entry_idx
                 << " - vehicle " << cmd->vehicleId);
        logEntriesCommitted_++;
        lastAppliedIndex_ = entry_idx;

        if (proposedTimes_.count(entry_idx)) {
            double delta = (NOW - proposedTimes_[entry_idx]).dbl();
            totalRaftDecisionTimeSec_ += delta;
            proposedTimes_.erase(entry_idx);
        }

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
    waitingForStatus_  = true;
    statusResponseCount_ = 0;
    collectedWayOfSight_.clear();
    collectedProposals_.clear();

    // Include our own wayOfSight
    collectedWayOfSight_[myId_] = wayOfSight_;
    auto myProp = buildMyProposal();
    collectedProposals_[myId_] = myProp;
}

// ============ STATUS REPORT PROPOSAL ============

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
        proposedTimes_[response.idx] = NOW;
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

// ============ PASS ORDER PROPOSAL (RAFT wrapper) ============
// The scheduling algorithm itself is in RaftDecision.cc::computePassOrder().

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

    // Call the decision algorithm (defined in RaftDecision.cc)
    PassScheduleEntry schedule = computePassOrder(collectedProposals_, activeVehicles_);

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

// ============ VEHICLE_LEFT PROPOSAL ============

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
    entry.type     = RAFT_LOGTYPE_REMOVE_NODE;
    entry.data.buf = entryData;
    entry.data.len = entrySize;

    msg_entry_response_t response;
    if (raft_recv_entry(raftServer_, &entry, &response) == 0) {
        logEntriesProposed_++;
        proposedTimes_[response.idx] = NOW;
    } else {
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
    for (int v : activeVehicles_) std::cout << v << " ";
    std::cout << "]" << std::endl;
    RAFT_LOG("BECAME LEADER (term=" << raft_get_current_term(raftServer_) << ")");
    wasElectedLeader_     = true;
    timeElected_          = NOW;
    failedElectionCount_  = 0;

    // Only propose status/pass order after 5s stopped (aggressive merge window)
    if (!hasStoppedAtIntersection_ || hasCommittedOrder_) return;

    double requiredWaitMs = discoveryWaitMs_ + clusterFormationDelayMs_;
    double waitedMs       = (NOW - timeStopped_).dbl() * 1000.0;
    double delayMs       = requiredWaitMs - waitedMs;

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
