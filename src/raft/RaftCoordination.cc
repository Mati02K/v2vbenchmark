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
#include "raft/RaftLogger.h"

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
    sendRaftBroadcast(/*COORD_PASS_ORDER_BROADCAST*/ 0x35, data);

    std::cout << simTime() << " [DBG][V" << myId_ << "] PASS_ORDER_BROADCAST sent (with QC): "
              << committedSchedule_.numBatches << " batches, QC round=" << prevRoundQC_.round
              << " numSigs=" << prevRoundQC_.numSigs << std::endl;
    RAFT_LOG("PASS_ORDER_BROADCAST sent (" << committedSchedule_.numBatches << " batches + QC)");
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
            for (int b = 0; b < qc.schedule.numBatches; b++)
                for (int v = 0; v < qc.schedule.batches[b].numVehicles; v++)
                    scheduledVehicles_.insert(qc.schedule.batches[b].vehicleIds[v]);
            std::cout << simTime() << " [QC][V" << myId_ << "] QC stored from PASS_ORDER_BROADCAST:"
                      << " round=" << qc.round << " numSigs=" << qc.numSigs << std::endl;
        }
    }

    RAFT_LOG("PASS_ORDER_BROADCAST applied (" << committedSchedule_.numBatches << " batches)");
    applyCommittedPassOrder();
}

// ============ PASS ORDER EXECUTION ============

void RaftAppBase::applyCommittedPassOrder()
{
    if (hasPassedIntersection_) return;

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
        RAFT_LOG("NOT SCHEDULED - using fallback");
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

    RAFT_LOG("assigned to batch " << myBatch_
             << " (current batch: " << currentBatch_ << ")");

    std::cout << simTime() << " [DBG][V" << myId_ << "] ASSIGNED batch=" << myBatch_
              << " currentBatch=" << currentBatch_
              << " hasCommitted=" << hasCommittedOrder_
              << " hasStopped=" << hasStoppedAtIntersection_ << std::endl;

    if (myBatch_ == 0) {
        std::cout << simTime() << " [DBG][V" << myId_ << "] BATCH 0 -> calling resumeMovement immediately" << std::endl;
        resumeMovement();
    } else if (!hasStoppedAtIntersection_ && traciVehicle_) {
        // Vehicle is still approaching — advance at full speed toward stop line so it
        // arrives ready instead of creeping in behind a stopped queue.
        try {
            double maxSpd = traciVehicle_->getMaxSpeed();
            if (maxSpd <= 0) maxSpd = 13.89;
            traciVehicle_->setSpeedMode(31);   // SUMO safety on (collision avoidance in lane)
            traciVehicle_->setSpeed(maxSpd);
            std::cout << simTime() << " [ADV][V" << myId_ << "] batch=" << myBatch_
                      << " advancing to stop line at max speed (dist="
                      << getDistanceToJunction() << "m)" << std::endl;
        } catch (...) {}
    }
    // Schedule 1.5s timeout: if VEHICLE_LEFT not received from vehicles in current batch, assume they left
    scheduleVehicleLeftTimeout(currentBatch_);

    if (myBatch_ != 0) {
        // Primary mechanism: checkBatchAdvance() will call resumeMovement() once
        // the previous batch has fully cleared via VEHICLE_LEFT broadcasts.
        // Safety fallback fires if VEHICLE_LEFT messages are lost or ghost vehicles
        // in the schedule never send them. 6 s per batch prevents endless waiting.
        int safetyDelayMs = myBatch_ * 6000;
        std::cout << simTime() << " [DBG][V" << myId_ << "] WAITING for batch " << myBatch_
                  << " (safetyFallback in " << safetyDelayMs << "ms)" << std::endl;
        scheduleOneshotMs(safetyDelayMs, [this, expectedBatch = myBatch_]() {
            if (!hasPassedIntersection_ && currentBatch_ < expectedBatch) {
                std::cout << simTime() << " [DBG][V" << myId_ << "] BATCH SAFETY TIMER: forcing batch "
                          << expectedBatch << " (lost VEHICLE_LEFT?)" << std::endl;
                RAFT_LOG("BATCH SAFETY FALLBACK (lost VEHICLE_LEFT?) for batch " << expectedBatch);
                currentBatch_ = expectedBatch;
                resumeMovement();
            }
        });
        RAFT_LOG("waiting for batch " << myBatch_ << " via checkBatchAdvance");
    }
}

// ============ VEHICLE PASSED / LEFT (follower-side) ============

void RaftAppBase::handleVehiclePassed(int vehicleId)
{
    std::cout << simTime() << " [DBG][V" << myId_ << "] VEHICLE_PASSED: V" << vehicleId
              << " frontVeh=" << vehicleInFrontOfMe_
              << " myBatch=" << myBatch_ << " curBatch=" << currentBatch_ << std::endl;
    RAFT_LOG("RECEIVED vehicle-passed from vehicle " << vehicleId);
    markRaftNodeInactive(vehicleId);
    activeVehicles_.erase(vehicleId);
    if (vehicleId == vehicleInFrontOfMe_) {
        wayOfSight_ = true;
        vehicleInFrontOfMe_ = -1;

        // Chain movement: when the vehicle in front of us passes, we must advance
        // toward the stop line. This applies BOTH to (a) vehicles not yet stopped,
        // AND (b) queued vehicles (hasStoppedAtIntersection_=true) waiting behind.
        if (!hasPassedIntersection_ && traciVehicle_ && timeStartedMoving_ == SIMTIME_ZERO) {
            std::cout << simTime() << " [CHAIN][V" << myId_ << "] Front V" << vehicleId
                      << " passed -> advancing" << std::endl;
            RAFT_LOG("front vehicle passed — advancing");
            try {
                // If we have a decision and it's our turn: go at full speed like resumeMovement().
                // Otherwise: restore car-following so SUMO advances us safely to the stop line.
                bool myTurn = hasCommittedOrder_ && myBatch_ != -1
                           && myBatch_ <= currentBatch_ && !hasPassedIntersection_;
                if (myTurn) {
                    std::cout << simTime() << " [CHAIN][V" << myId_ << "] it's our turn (batch="
                              << myBatch_ << ") — resuming at max speed" << std::endl;
                    resumeMovement();
                } else {
                    traciVehicle_->setSpeedMode(31);
                    traciVehicle_->setSpeed(-1);
                }
            } catch (...) {}
        }
    }
}

void RaftAppBase::handleVehicleLeft(int vehicleId, int batchId)
{
    if (gossipSeenVehicleLeft_.count(vehicleId)) return;
    gossipSeenVehicleLeft_.insert(vehicleId);

    std::cout << simTime() << " [DBG][V" << myId_ << "] VEHICLE_LEFT: V" << vehicleId
              << " batch=" << batchId
              << " curBatch=" << currentBatch_
              << " vehiclesLeftSoFar=[";
    for (int v : vehiclesLeftInBatch_) std::cout << v << " ";
    std::cout << "]" << std::endl;
    RAFT_LOG("RECEIVED vehicle-left from vehicle " << vehicleId);

    markRaftNodeInactive(vehicleId);
    activeVehicles_.erase(vehicleId);
    vehiclesLeftInBatch_.insert(vehicleId);
    checkBatchAdvance();

    // Remove from vehicleDB_ after 2 seconds so lane leader flag stays fresh
    scheduleOneshotMs(2000.0, [this, vehicleId]() {
        vehicleDB_.erase(vehicleId);
        std::cout << simTime() << " [DBG][V" << myId_ << "] vehicleDB_ cleaned V"
                  << vehicleId << " (2s post-exit)" << std::endl;
    });
}

// ============ SEND EXIT NOTIFICATIONS ============

void RaftAppBase::sendVehiclePassed()
{
    RAFT_LOG("BROADCASTING vehicle-passed");
    std::vector<uint8_t> data = {static_cast<uint8_t>(myId_)};
    sendRaftBroadcast(/*COORD_VEHICLE_PASSED*/ 0x33, data);
}

void RaftAppBase::sendVehicleLeft()
{
    RAFT_LOG("sending vehicle-left (broadcast, isLeader=" << isLeader_
             << ", myBatch=" << myBatch_ << ")");
    VehicleLeftEntry e; e.vehicleId = myId_; e.batchId = myBatch_;
    std::vector<uint8_t> data(sizeof(VehicleLeftEntry));
    memcpy(data.data(), &e, sizeof(e));
    sendRaftBroadcast(/*COORD_VEHICLE_LEFT*/ 0x34, data);
}

// ============ VEHICLE_LEFT TIMEOUT ============

void RaftAppBase::scheduleVehicleLeftTimeout(int batchIndex)
{
    if (batchIndex >= committedSchedule_.numBatches) return;
    int timeoutMs = vehicleLeftTimeoutMs_;
    scheduleOneshotMs(timeoutMs, [this, batchForTimeout = batchIndex]() {
        if (hasPassedIntersection_ || !hasCommittedOrder_) return;
        if (currentBatch_ != batchForTimeout) return;  // already advanced past this batch
        if (batchForTimeout >= (int)committedSchedule_.numBatches) return;

        PassBatch& batch = committedSchedule_.batches[batchForTimeout];
        bool anyAdded = false;
        for (int v = 0; v < batch.numVehicles; v++) {
            int vid = batch.vehicleIds[v];
            if (!vehiclesLeftInBatch_.count(vid)) {
                RAFT_LOG("VEHICLE_LEFT timeout: assuming V" << vid << " left (batch " << batchForTimeout << ")");
                std::cout << simTime() << " [DBG][V" << myId_ << "] VEHICLE_LEFT TIMEOUT: assuming V"
                          << vid << " left (batch " << batchForTimeout << ")" << std::endl;
                markRaftNodeInactive(vid);
                activeVehicles_.erase(vid);
                vehiclesLeftInBatch_.insert(vid);
                gossipSeenVehicleLeft_.insert(vid);
                anyAdded = true;
            }
        }
        if (anyAdded) checkBatchAdvance();
    });
}

// ============ BATCH ADVANCEMENT ============

void RaftAppBase::checkBatchAdvance()
{
    if (hasPassedIntersection_ || !hasCommittedOrder_) return;
    if (currentBatch_ >= committedSchedule_.numBatches) return;

    PassBatch& batch = committedSchedule_.batches[currentBatch_];
    bool allLeft = true;
    std::string missing = "";
    for (int v = 0; v < batch.numVehicles; v++) {
        int vid = batch.vehicleIds[v];
        if (!vehiclesLeftInBatch_.count(vid)) {
            allLeft = false;
            missing += std::to_string(vid) + " ";
        }
    }

    std::cout << simTime() << " [DBG][V" << myId_ << "] CHECK_BATCH_ADVANCE curBatch=" << currentBatch_
              << " allLeft=" << allLeft
              << " missing=[" << missing << "]"
              << " vehiclesLeft=[";
    for (int v : vehiclesLeftInBatch_) std::cout << v << " ";
    std::cout << "]" << std::endl;

    if (allLeft) {
        currentBatch_++;
        vehiclesLeftInBatch_.clear();
        RAFT_LOG("BATCH ADVANCE: now on batch " << currentBatch_);
        std::cout << simTime() << " [DBG][V" << myId_ << "] BATCH ADVANCED to " << currentBatch_
                  << " myBatch=" << myBatch_ << std::endl;
        if (myBatch_ == currentBatch_ && !hasPassedIntersection_) {
            RAFT_LOG("MY BATCH - starting movement");
            std::cout << simTime() << " [DBG][V" << myId_ << "] MY BATCH NOW ACTIVE -> resumeMovement()" << std::endl;
            resumeMovement();
        }
        // Schedule 1.5s timeout for new current batch (if not past last batch)
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
        RAFT_LOG("LEFT intersection");
        sendVehiclePassed();
        sendVehicleLeft();
        outputMetricsJSON();
    }
}
