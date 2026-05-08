// RaftCoordination.cc — Send committed schedule + QC to all vehicles, then
//                       execute the crossing pass (batch management, vehicle exit).
//
// Flow:
//   tryAssembleQC() (RaftCore) calls sendPassOrderBroadcast() here once QC is ready.
//   Non-cluster vehicles receive the schedule via handlePassOrderBroadcast().
//   applyCommittedPassOrder() assigns each vehicle to its batch and starts movement.
//   Batch advancement is driven by pollBatchExit() querying TraCI every 100ms.
//   checkIfLeftIntersection() detects own physical exit and writes metrics.

#include "raft/RaftAppBase.h"
#include "raft/RaftTypes_m.h"

#include <cstring>
#include <iostream>

#define NOW (simTime())

// ============ PASS ORDER BROADCAST (leader → all vehicles) ============

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
    sendRaftBroadcast(benchmark::COORD_PASS_ORDER_BROADCAST, data);

    std::cout << simTime() << " [DBG][V" << myId_ << "] PASS_ORDER_BROADCAST sent (with QC): "
              << committedSchedule_.numBatches << " batches, QC round=" << prevRoundQC_.round
              << " numSigs=" << prevRoundQC_.numSigs << std::endl;
}

// Non-cluster (queued) vehicles receive the committed schedule + QC here.
// Payload: [PassScheduleEntry][QuorumCertificate]
// RAFT cluster members apply the schedule via doApplyLog() — they skip this.
void RaftAppBase::handlePassOrderBroadcast(const std::vector<uint8_t>& data)
{
    if (hasPassedIntersection_) return;
    if (hasCommittedOrder_) return;  // already have the schedule (duplicate broadcast)
    if (data.size() < sizeof(PassScheduleEntry)) {
        std::cout << simTime() << " [WARN][V" << myId_
                  << "] PASS_ORDER_BROADCAST: payload too small (" << data.size()
                  << " < " << sizeof(PassScheduleEntry) << ")" << std::endl;
        return;
    }

    // Gossip relay: rebroadcast once so vehicles further back in the queue also learn.
    sendRaftBroadcast(benchmark::COORD_PASS_ORDER_BROADCAST, data);

    PassScheduleEntry schedule;
    memcpy(&schedule, data.data(), sizeof(PassScheduleEntry));

    std::cout << simTime() << " [DBG][V" << myId_ << "] PASS_ORDER_BROADCAST received: "
              << schedule.numBatches << " batches (raftServer_=" << (raftServer_ != nullptr) << ")" << std::endl;

    memcpy(&committedSchedule_, &schedule, sizeof(PassScheduleEntry));
    hasCommittedOrder_ = true;
    coordinationMethod_ = "raft";

    // Extract embedded QC if present.
    if (data.size() >= sizeof(PassScheduleEntry) + sizeof(QuorumCertificate)) {
        QuorumCertificate qc;
        memcpy(&qc, data.data() + sizeof(PassScheduleEntry), sizeof(QuorumCertificate));
        if (qc.valid && qc.numSigs > 0 && (!hasPrevRoundQC_ || qc.round > prevRoundQC_.round)) {
            prevRoundQC_    = qc;
            hasPrevRoundQC_ = true;
            qcAssembled_    = true;
            for (int b = 0; b < qc.schedule.numBatches; b++) {
                for (int v = 0; v < qc.schedule.batches[b].numVehicles; v++) {
                    scheduledVehicles_.insert(qc.schedule.batches[b].vehicleIds[v]);
                }
            }
            std::cout << simTime() << " [QC][V" << myId_ << "] QC stored from PASS_ORDER_BROADCAST:"
                      << " round=" << qc.round << " numSigs=" << qc.numSigs << std::endl;
        }
    }

    applyCommittedPassOrder();
}

// ============ PASS ORDER EXECUTION ============

void RaftAppBase::applyCommittedPassOrder()
{
    if (hasPassedIntersection_) return;

    // Find my batch from the list
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
        std::cout << simTime() << " [DBG][V" << myId_ << "] NOT IN SCHEDULE! Using fallback." << std::endl;
        int fallbackDelayMs = committedSchedule_.numBatches * 5000 + 10000;
        scheduleOneshotMs(fallbackDelayMs, [this]() {
            if (!hasPassedIntersection_) {
                isFallbackMode_     = true;
                coordinationMethod_ = "fallback";
                resumeMovement();
            }
        });
        return;
    }

    std::cout << simTime() << " [DBG][V" << myId_ << "] ASSIGNED batch=" << myBatch_
              << " currentBatch=" << currentBatch_
              << " hasCommitted=" << hasCommittedOrder_
              << " hasStopped=" << hasStoppedAtIntersection_ << std::endl;

    if (myBatch_ == 0) {
        std::cout << simTime() << " [DBG][V" << myId_ << "] BATCH 0 -> calling resumeMovement immediately" << std::endl;
        resumeMovement();
    }
    scheduleOneshotMs(batchExitPollMs_, [this]() { pollBatchExit(); });

    if (myBatch_ != 0) {
        std::cout << simTime() << " [DBG][V" << myId_ << "] WAITING for batch " << myBatch_ << std::endl;
    }
}

// ============ BATCH EXIT POLLING (TraCI-based) ============

void RaftAppBase::pollBatchExit()
{
    if (hasPassedIntersection_ || !hasCommittedOrder_) return;
    if (currentBatch_ >= committedSchedule_.numBatches) return;

    PassBatch& batch = committedSchedule_.batches[currentBatch_];
    for (int v = 0; v < batch.numVehicles; v++) {
        int vid = batch.vehicleIds[v];
        if (vehiclesLeftInBatch_.count(vid)) {
            continue;
        }
        bool hasLeft = false;
        if (!sumoIdMap_.count(vid)) {
            // We never heard a beacon from this peer (likely never came in radio range),
            // so we cannot query TraCI for its road. Presume left — SUMO car-following
            // on our own lane prevents us from physically running into anyone.
            std::cout << simTime() << " [WARN][V" << myId_ << "] pollBatchExit: no SUMO ID for vid="
                      << vid << " — presuming left (batch=" << currentBatch_ << ")" << std::endl;
            hasLeft = true;
        } else {
            try {
                std::string roadId = traci_->vehicle(sumoIdMap_.at(vid)).getRoadId();
                hasLeft = exitEdges_.count(roadId) > 0
                       || roadId.empty()
                       || (!intersectionEdges_.count(roadId) && roadId[0] != ':');
            } catch (...) {
                std::cout << simTime() << " [WARN][V" << myId_ << "] pollBatchExit: TraCI query failed for vid=" << vid << " sumoId=" << sumoIdMap_.at(vid) << std::endl;
                hasLeft = true;
            }
        }
        if (hasLeft) {
            std::cout << simTime() << " [DBG][V" << myId_ << "] POLL: V" << vid
                      << " cleared intersection (batch=" << currentBatch_ << ")" << std::endl;
            vehiclesLeftInBatch_.insert(vid);
            markRaftNodeInactive(vid);
            activeVehicles_.erase(vid);
            scheduleOneshotMs(2000.0, [this, vid]() { vehicleDB_.erase(vid); });
        }
    }

    bool allLeft = true;
    for (int v = 0; v < batch.numVehicles; v++) {
        if (!vehiclesLeftInBatch_.count(batch.vehicleIds[v])) {
            allLeft = false;
            break;
        }
    }

    if (allLeft) {
        currentBatch_++;
        vehiclesLeftInBatch_.clear();
        std::cout << simTime() << " [DBG][V" << myId_ << "] BATCH ADVANCED to " << currentBatch_
                  << " myBatch=" << myBatch_ << std::endl;
        if (myBatch_ == currentBatch_ && !hasPassedIntersection_) {
            if (allowMultipleRounds_ && !seekingNewCluster_) {
                std::cout << simTime() << " [DBG][V" << myId_ << "] MY BATCH NOW ACTIVE (multirounds) -> startNewRound()" << std::endl;
                startNewRound();
                return;
            }
            std::cout << simTime() << " [DBG][V" << myId_ << "] MY BATCH NOW ACTIVE -> resumeMovement()" << std::endl;
            resumeMovement();
        }
    }

    if (!hasPassedIntersection_ && currentBatch_ < committedSchedule_.numBatches) {
        scheduleOneshotMs(batchExitPollMs_, [this]() { pollBatchExit(); });
    }
}

// ============ INTERSECTION EXIT DETECTION ============

void RaftAppBase::checkIfLeftIntersection()
{
    if (!hasPassedIntersection_ && hasPassedIntersectionEdge()) {
        hasPassedIntersection_ = true;
        timePassed_            = NOW;
        myLaneIndex_ = -1;  // no longer on approach lane
        try { prevRoadId_ = traciVehicle_->getRoadId(); } catch (...) {}
        std::cout << simTime() << " [DBG][V" << myId_ << "] LEFT intersection" << std::endl;
        outputMetricsJSON();
    }
}
