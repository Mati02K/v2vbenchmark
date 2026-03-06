// RaftExit.cc — Vehicle exit notifications and batch advancement
// All messages sent when a vehicle finishes crossing, plus metrics output.

#include "raft/RaftAppBase.h"
#include "raft/RaftLogger.h"

#include <cstring>
#include <iostream>

#define NOW (simTime())

// ============ SEND NOTIFICATIONS ============

void RaftAppBase::sendVehiclePassed()
{
    RAFT_LOG("BROADCASTING vehicle-passed");
    std::vector<uint8_t> data = {static_cast<uint8_t>(myId_)};
    sendRaftBroadcast(/*COORD_VEHICLE_PASSED*/ 0x33, data);
    messagesSent_++;
}

void RaftAppBase::sendVehicleLeft()
{
    RAFT_LOG("sending vehicle-left (broadcast, isLeader=" << isLeader_
             << ", myBatch=" << myBatch_ << ")");
    VehicleLeftEntry e; e.vehicleId = myId_; e.batchId = myBatch_;
    std::vector<uint8_t> data(sizeof(VehicleLeftEntry));
    memcpy(data.data(), &e, sizeof(e));
    sendRaftBroadcast(/*COORD_VEHICLE_LEFT*/ 0x34, data);
    messagesSent_++;
}

// ============ RAFT NODE MANAGEMENT ============

void RaftAppBase::markRaftNodeInactive(int vehicleId)
{
    if (vehicleId == myId_) return;
    if (raftServer_) {
        raft_node_t* node = raft_get_node(raftServer_, getNodeIdFromVehicleId(vehicleId));
        if (node) {
            raft_node_set_active(node, 0);
            RAFT_LOG("RAFT node for vehicle " << vehicleId << " marked INACTIVE (left intersection)");
        }
    } else {
        /* We haven't formed yet; record for when we do (late-joiner case). */
        vehiclesLeftBeforeFormed_.insert(vehicleId);
    }
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

// ============ METRICS OUTPUT ============

void RaftAppBase::outputMetricsJSON()
{
    if (metricsWritten_) return;
    metricsWritten_ = true;

    // RAFT decision time = status collection + sum(commit - propose); leader election excluded
    double raftDecisionMs = statusCollectionTimeMs_ + (totalRaftDecisionTimeSec_ * 1000.0);
    // Only the leader who proposed the decision contributes (sub-cluster leaders that merged reset)
    if (logEntriesProposed_ == 0) raftDecisionMs = 0.0;

    RaftMetrics::writeVehicleJSON(
        myId_,
        myLane_,
        myRoute_,
        wasElectedLeader_,
        coordinationMethod_,
        transportName_,
        timeStopped_.dbl()       * 1000.0,
        timeClusterFormed_.dbl() * 1000.0,
        timeElected_.dbl()       * 1000.0,
        timeOrderCommitted_.dbl()* 1000.0,
        timeStartedMoving_.dbl() * 1000.0,
        timePassed_.dbl()        * 1000.0,
        raftDecisionMs,
        messagesSent_,
        messagesReceived_,
        electionRounds_,
        logEntriesProposed_,
        logEntriesCommitted_,
        myBatch_
    );
}
