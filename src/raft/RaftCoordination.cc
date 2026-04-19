// RaftCoordination.cc — Send committed schedule + QC to all vehicles, then
//                       execute the crossing pass (batch management, vehicle exit).
//
// Flow:
//   tryAssembleQC() (RaftCore) calls sendPassOrderBroadcast() here once QC is ready.
//   Non-cluster vehicles receive the schedule via handlePassOrderBroadcast().
//   applyCommittedPassOrder() assigns each vehicle to its batch and starts movement.
//   Batch advancement is driven by VEHICLE_LEFT messages and checkBatchAdvance().
//   checkIfLeftIntersection() detects physical exit → sends notifications + writes metrics.

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

    if (hasStoppedAtIntersection_ && timeStopped_ > SIMTIME_ZERO)
        timeOrderCommitted_ = NOW;

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
    scheduleVehicleLeftTimeout(currentBatch_);

    if (myBatch_ != 0) {
        std::cout << simTime() << " [DBG][V" << myId_ << "] WAITING for batch " << myBatch_ << std::endl;
    }
}

// ============ VEHICLE PASSED / LEFT (follower-side) ============

void RaftAppBase::handleVehicleLeft(int vehicleId, int batchId)
{
    if (vehiclesLeftInBatch_.count(vehicleId)) return;

    // Gossip relay: rebroadcast once so vehicles further back in the queue also learn.
    VehicleLeftEntry relay; 
    relay.vehicleId = vehicleId; 
    relay.batchId = batchId;
    std::vector<uint8_t> relayData(sizeof(VehicleLeftEntry));
    memcpy(relayData.data(), &relay, sizeof(relay));
    sendRaftBroadcast(benchmark::COORD_VEHICLE_LEFT, relayData);

    std::cout << simTime() << " [DBG][V" << myId_ << "] VEHICLE_LEFT: V" << vehicleId
              << " batch=" << batchId
              << " curBatch=" << currentBatch_
              << " vehiclesLeftSoFar=[";
    for (int v : vehiclesLeftInBatch_) std::cout << v << " ";
    std::cout << "]" << std::endl;

    markRaftNodeInactive(vehicleId);
    activeVehicles_.erase(vehicleId);
    vehiclesLeftInBatch_.insert(vehicleId);
    checkBatchAdvance();

    scheduleOneshotMs(2000.0, [this, vehicleId]() {
        vehicleDB_.erase(vehicleId);
    });
}

// ============ SEND EXIT NOTIFICATION ============

void RaftAppBase::sendVehicleLeft()
{
    std::cout << simTime() << " [DBG][V" << myId_ << "] VEHICLE_LEFT broadcast (batch=" << myBatch_ << ")" << std::endl;
    VehicleLeftEntry e; e.vehicleId = myId_; e.batchId = myBatch_;
    std::vector<uint8_t> data(sizeof(VehicleLeftEntry));
    memcpy(data.data(), &e, sizeof(e));
    sendRaftBroadcast(benchmark::COORD_VEHICLE_LEFT, data);
}

// ============ VEHICLE_LEFT TIMEOUT ============

void RaftAppBase::onVehicleLeftTimeout(int batchIndex)
{
    if (hasPassedIntersection_ || !hasCommittedOrder_) return;
    if (currentBatch_ != batchIndex) return;
    if (batchIndex >= (int)committedSchedule_.numBatches) return;

    PassBatch& batch = committedSchedule_.batches[batchIndex];
    for (int v = 0; v < batch.numVehicles; v++) {
        int vid = batch.vehicleIds[v];
        if (!vehiclesLeftInBatch_.count(vid)) {
            handleVehicleLeft(vid, batchIndex);
        }
    }
}

void RaftAppBase::scheduleVehicleLeftTimeout(int batchIndex)
{
    if (batchIndex >= committedSchedule_.numBatches) return;
    scheduleOneshotMs(vehicleLeftTimeoutMs_, [this, batchForTimeout = batchIndex]() {
        onVehicleLeftTimeout(batchForTimeout);
    });
}

// ============ BATCH ADVANCEMENT ============

void RaftAppBase::checkBatchAdvance()
{
    if (hasPassedIntersection_ || !hasCommittedOrder_) return;
    // All batches already processed — nothing left to advance.
    if (currentBatch_ >= committedSchedule_.numBatches) return;

    // Check whether every vehicle in the current batch has reported leaving.
    PassBatch& batch = committedSchedule_.batches[currentBatch_];
    bool allLeft = true;
    for (int v = 0; v < batch.numVehicles; v++) {
        if (!vehiclesLeftInBatch_.count(batch.vehicleIds[v])) {
            allLeft = false;
            break;
        }
    }

    if (allLeft) {
        // Advance to next batch and reset per-batch tracking.
        currentBatch_++;
        vehiclesLeftInBatch_.clear();
        std::cout << simTime() << " [DBG][V" << myId_ << "] BATCH ADVANCED to " << currentBatch_
                  << " myBatch=" << myBatch_ << std::endl;

        // If this vehicle belongs to the newly active batch, start moving.
        if (myBatch_ == currentBatch_ && !hasPassedIntersection_) {
            std::cout << simTime() << " [DBG][V" << myId_ << "] MY BATCH NOW ACTIVE -> resumeMovement()" << std::endl;
            resumeMovement();
        }

        // Arm a timeout in case some vehicles in the new batch never broadcast VEHICLE_LEFT.
        if (currentBatch_ < committedSchedule_.numBatches) {
            scheduleVehicleLeftTimeout(currentBatch_);
        }
    }
}

// ============ INTERSECTION EXIT DETECTION ============

void RaftAppBase::checkIfLeftIntersection()
{
    if (!hasPassedIntersection_ && hasPassedIntersectionEdge()) {
        hasPassedIntersection_ = true;
        timePassed_            = NOW;
        std::cout << simTime() << " [DBG][V" << myId_ << "] LEFT intersection" << std::endl;
        sendVehicleLeft();
        outputMetricsJSON();
    }
}
