// RaftDecision.cc — Crossing-order scheduling algorithm
// This is the only file you need to edit to change the intersection pass policy.
// computePassOrder() is called by proposePassOrder() in RaftCore.cc.

#include "raft/RaftAppBase.h"
#include "raft/RaftLogger.h"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <set>
#include <vector>

#define NOW (simTime())

// ============ LANE / TURN HELPERS ============

int RaftAppBase::getLaneIndex(const std::string& lane)
{
    if (lane == "N2C") return 3;  // North (swapped with West)
    if (lane == "S2C") return 1;
    if (lane == "E2C") return 2;
    if (lane == "W2C") return 0;  // West (swapped with North)
    return 0;
}

bool RaftAppBase::movementsConflict(int laneA, int turnA, int laneB, int turnB)
{
    if (laneA == laneB) return true;
    bool opposing = (laneA + laneB == 2) || (laneA + laneB == 4);
    if (opposing && turnA == 0 && turnB == 0) return false;
    if (opposing && turnA == 1 && turnB == 1) return false;
    return true;
}

// ============ DECISION ALGORITHM ============
// Takes all collected proposals and returns the batch schedule.
// Edit this function to change crossing priority / batch-building policy.

PassScheduleEntry RaftAppBase::computePassOrder(
    const std::map<int, VehicleProposal>& proposals,
    const std::set<int>& /*activeVehicles*/)
{
    // Sort pool: front vehicles first, then by wait time (fairness), lane, distance
    std::vector<VehicleProposal> pool;
    for (const auto& kv : proposals) pool.push_back(kv.second);

    std::sort(pool.begin(), pool.end(), [](const VehicleProposal& a, const VehicleProposal& b) {
        if (a.isFirstInLane != b.isFirstInLane)  return a.isFirstInLane > b.isFirstInLane;
        double waitDiff = a.waitingTimeMs - b.waitingTimeMs;
        if (std::abs(waitDiff) > 500.0) return waitDiff > 0;
        if (a.laneIndex != b.laneIndex)  return a.laneIndex < b.laneIndex;
        return a.distanceToJunction < b.distanceToJunction;
    });

    PassScheduleEntry schedule;
    memset(&schedule, 0, sizeof(schedule));
    std::set<int> scheduled;

    auto blockerScheduled = [&](const VehicleProposal& v) {
        return (v.blockedByVehicleId < 0) || scheduled.count(v.blockedByVehicleId) > 0;
    };

    while (!pool.empty() && schedule.numBatches < 16) {
        PassBatch& batch = schedule.batches[schedule.numBatches];

        auto primaryIt = pool.begin();
        while (primaryIt != pool.end() && !blockerScheduled(*primaryIt)) ++primaryIt;
        if (primaryIt == pool.end()) primaryIt = pool.begin();

        VehicleProposal primary = *primaryIt;
        batch.vehicleIds[batch.numVehicles++] = primary.vehicleId;
        scheduled.insert(primary.vehicleId);
        pool.erase(primaryIt);

        for (auto it = pool.begin(); it != pool.end() && batch.numVehicles < 8; ) {
            if (!blockerScheduled(*it)) { ++it; continue; }
            bool conflict = false;
            for (int i = 0; i < batch.numVehicles && !conflict; i++) {
                int existingId = batch.vehicleIds[i];
                int lA = proposals.count(existingId) ? proposals.at(existingId).laneIndex : 0;
                int tA = proposals.count(existingId) ? proposals.at(existingId).intendedTurn : 0;
                int lB = it->laneIndex;
                int tB = it->intendedTurn;
                if (movementsConflict(lA, tA, lB, tB)) {
                    conflict = true;
                }
            }
            if (!conflict) {
                batch.vehicleIds[batch.numVehicles++] = it->vehicleId;
                scheduled.insert(it->vehicleId);
                it = pool.erase(it);
            } else ++it;
        }
        schedule.numBatches++;
    }

    return schedule;
}

// ============ PASS ORDER EXECUTION ============

void RaftAppBase::executePassOrder()
{
    applyCommittedPassOrder();
}

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
            std::cout << simTime() << " [DBG][V" << myId_ << "] BATCH SAFETY FALLBACK FIRED for batch "
                      << expectedBatch << " curBatch=" << currentBatch_
                      << " hasPassedIntersection=" << hasPassedIntersection_ << std::endl;
            if (!hasPassedIntersection_ && currentBatch_ < expectedBatch) {
                RAFT_LOG("BATCH SAFETY FALLBACK (lost VEHICLE_LEFT?) for batch " << expectedBatch);
                currentBatch_ = expectedBatch;
                resumeMovement();
            }
        });
        RAFT_LOG("waiting for batch " << myBatch_ << " via checkBatchAdvance");
    }
}
