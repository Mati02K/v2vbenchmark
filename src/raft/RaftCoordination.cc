// RaftCoordination.cc — Send committed schedule to all vehicles, then
//                       execute the crossing pass (batch management, vehicle exit).
//
// Flow:
//   doApplyLog() (RaftCore) calls sendPassOrderBroadcast() here once the schedule
//   is committed by RAFT consensus. Non-cluster vehicles receive the schedule via
//   handlePassOrderBroadcast(). applyCommittedPassOrder() assigns each vehicle to
//   its batch and starts movement. Batch advancement is driven by pollBatchExit()
//   querying TraCI every 100ms. checkIfLeftIntersection() detects own physical
//   exit and writes metrics.

#include "raft/RaftAppBase.h"
#include "raft/RaftTypes_m.h"

#include <cstring>
#include <iostream>

#define NOW (simTime())

// ============ PASS ORDER BROADCAST (leader → all vehicles) ============

// Called by doApplyLog() on the leader after RAFT commits the schedule entry.
// Used by non-cluster (queued) vehicles in cluster mode to learn the schedule.
// RAFT cluster members already have it via doApplyLog. Authenticity comes from
// the message-level cert verification on every receive (no separate QC layer).
// Payload: [uint32_t round || PassScheduleEntry schedule]
void RaftAppBase::sendPassOrderBroadcast()
{
    if (!hasCommittedOrder_) return;

    size_t payloadSize = sizeof(uint32_t) + sizeof(PassScheduleEntry);
    std::vector<uint8_t> data(payloadSize);
    uint32_t rn = static_cast<uint32_t>(roundNumber_);
    memcpy(data.data(),                    &rn,                 sizeof(uint32_t));
    memcpy(data.data() + sizeof(uint32_t), &committedSchedule_, sizeof(PassScheduleEntry));

    sendRaftBroadcast(benchmark::COORD_PASS_ORDER_BROADCAST, data);

    std::cout << simTime() << " [DBG][V" << myId_ << "] PASS_ORDER_BROADCAST sent: "
              << committedSchedule_.numBatches << " batches, round=" << rn << std::endl;
}

// Non-cluster (queued) vehicles receive the committed schedule here.
// Payload: [uint32_t round || PassScheduleEntry schedule]
// RAFT cluster members apply the schedule via doApplyLog() — they skip this.
void RaftAppBase::handlePassOrderBroadcast(const std::vector<uint8_t>& data)
{
    if (hasPassedIntersection_) return;
    if (hasCommittedOrder_) return;
    if (data.size() < sizeof(uint32_t) + sizeof(PassScheduleEntry)) {
        std::cout << simTime() << " [WARN][V" << myId_
                  << "] PASS_ORDER_BROADCAST: payload too small (" << data.size()
                  << " < " << sizeof(uint32_t) + sizeof(PassScheduleEntry) << ")" << std::endl;
        return;
    }

    // In cluster mode, lane leaders relay to queued followers.
    // In allVehicles mode, gossipIfNew in the dispatch handles this with dedup.
    if (clusterMode_ == "cluster") {
        sendRaftBroadcast(benchmark::COORD_PASS_ORDER_BROADCAST, data);
    }

    PassScheduleEntry schedule;
    memcpy(&schedule,
           data.data() + sizeof(uint32_t),
           sizeof(PassScheduleEntry));

    std::cout << simTime() << " [DBG][V" << myId_ << "] PASS_ORDER_BROADCAST received: "
              << schedule.numBatches << " batches" << std::endl;

    memcpy(&committedSchedule_, &schedule, sizeof(PassScheduleEntry));
    hasCommittedOrder_ = true;
    coordinationMethod_ = "raft";

    for (int b = 0; b < schedule.numBatches; b++) {
        for (int v = 0; v < schedule.batches[b].numVehicles; v++) {
            scheduledVehicles_.insert(schedule.batches[b].vehicleIds[v]);
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
            clusterVehicles_.erase(vid);
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
