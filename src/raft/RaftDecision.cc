// RaftDecision.cc — Crossing-order scheduling algorithm
// This is the only file you need to edit to change the intersection pass policy.
// computePassOrder() is called by proposePassOrder() in RaftCore.cc.
// applyCommittedPassOrder() has moved to RaftCoordination.cc (execution, not algorithm).

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
//
// Priority policy (priority vehicle-aware):
//   Step 1 — Find "priority lanes": any lane that contains at least one vehicle
//             with isPriority=true (verified by Emergency_CA cert).
//   Step 2 — Schedule ALL vehicles from priority lanes first, round-robining
//             between priority lanes if more than one. Within each priority lane,
//             vehicles go in queue order (blocker before blocked).
//             This continues until the priority vehicle itself is scheduled.
//   Step 3 — Schedule remaining (non-priority-lane) vehicles using the normal
//             fairness algorithm (wait time, distance, lane).
//   If no priority lanes exist, skip to Step 3 directly.

PassScheduleEntry RaftAppBase::computePassOrder(
    const std::map<int, VehicleProposal>& proposals,
    const std::set<int>& /*activeVehicles*/)
{
    PassScheduleEntry schedule;
    memset(&schedule, 0, sizeof(schedule));
    std::set<int> scheduled;

    auto blockerScheduled = [&](const VehicleProposal& v) {
        return (v.blockedByVehicleId < 0) || scheduled.count(v.blockedByVehicleId) > 0;
    };

    // Helper: add one vehicle to the current batch; open a new batch if there's a conflict.
    auto addToBatch = [&](const VehicleProposal& v) {
        while (schedule.numBatches < 16) {
            PassBatch& b = schedule.batches[schedule.numBatches];
            bool conflict = false;
            for (int i = 0; i < b.numVehicles && !conflict; i++) {
                int eid = b.vehicleIds[i];
                int lA  = proposals.count(eid) ? proposals.at(eid).laneIndex   : 0;
                int tA  = proposals.count(eid) ? proposals.at(eid).intendedTurn : 0;
                if (movementsConflict(lA, tA, v.laneIndex, v.intendedTurn)) conflict = true;
            }
            if (!conflict && b.numVehicles < 8) {
                b.vehicleIds[b.numVehicles++] = v.vehicleId;
                scheduled.insert(v.vehicleId);
                return;
            }
            // Current batch is full or has a conflict — open a new one.
            if (b.numVehicles > 0) schedule.numBatches++;
        }
    };

    // ---- Step 1: identify priority lanes ----
    std::set<int> priorityLanes;
    for (const auto& kv : proposals)
    {
        if (kv.second.isPriority)
        {
            priorityLanes.insert(kv.second.laneIndex);
        }
    }

    if (!priorityLanes.empty()) {
        std::cout << "[PRIORITY] Priority vehicle detected in lane(s): ";
        for (int l : priorityLanes)
        {
            std::cout << l << " ";
        }
        std::cout << std::endl;
    }

    // ---- Step 2: schedule priority lanes (round-robin between them) ----
    // For each priority lane: schedule vehicles in queue order (closest to junction first)
    // until the priority vehicle in that lane is scheduled. Then that lane is "done".

    // Build per-lane vehicle lists (sorted by distance, closest first)
    std::map<int, std::vector<VehicleProposal>> laneVehicles;
    for (const auto& kv : proposals) {
        if (priorityLanes.count(kv.second.laneIndex))
        {
            laneVehicles[kv.second.laneIndex].push_back(kv.second);
        }
    }
    for (auto& kv : laneVehicles) {
        std::sort(kv.second.begin(), kv.second.end(),
                  [](const VehicleProposal& a, const VehicleProposal& b) {
                      return a.distanceToJunction < b.distanceToJunction;
                  });
    }

    // Track which priority lanes still have their priority vehicle unscheduled
    std::set<int> activePriorityLanes = priorityLanes;
    // Round-robin index across priority lanes
    std::vector<int> prioLaneOrder(priorityLanes.begin(), priorityLanes.end());

    bool progress = true;
    while (!activePriorityLanes.empty() && progress) {
        progress = false;
        for (int lane : prioLaneOrder) {
            if (!activePriorityLanes.count(lane)) continue;
            auto& lvec = laneVehicles[lane];
            // Schedule the next unscheduled vehicle in this lane
            for (auto it = lvec.begin(); it != lvec.end(); ) {
                if (scheduled.count(it->vehicleId)) { it = lvec.erase(it); continue; }
                if (!blockerScheduled(*it)) { ++it; continue; }
                bool wasAmb = it->isPriority;
                addToBatch(*it);
                it = lvec.erase(it);
                progress = true;
                if (wasAmb) {
                    // Priority vehicle in this lane has been scheduled — lane loses priority
                    activePriorityLanes.erase(lane);
                    std::cout << "[PRIORITY] Priority vehicle in lane " << lane
                              << " scheduled — lane priority released." << std::endl;
                }
                break; // one vehicle per lane per round-robin turn
            }
            if (lvec.empty()) activePriorityLanes.erase(lane);
        }
    }
    // Flush any remaining batches from priority lanes (vehicles after the priority vehicle)
    // They go into the pool for the normal algorithm below.

    // ---- Step 3: normal fairness algorithm for remaining vehicles ----
    std::vector<VehicleProposal> pool;
    for (const auto& kv : proposals) {
        if (!scheduled.count(kv.second.vehicleId))
        {
            pool.push_back(kv.second);
        }
    }

    std::sort(pool.begin(), pool.end(), [](const VehicleProposal& a, const VehicleProposal& b) {
        if (a.isFirstInLane != b.isFirstInLane) return a.isFirstInLane > b.isFirstInLane;
        double waitDiff = a.waitingTimeMs - b.waitingTimeMs;
        if (std::abs(waitDiff) > 500.0) return waitDiff > 0;
        if (a.laneIndex != b.laneIndex) return a.laneIndex < b.laneIndex;
        return a.distanceToJunction < b.distanceToJunction;
    });

    // Ensure current batch is committed before starting normal pool
    if (schedule.numBatches < 16 && schedule.batches[schedule.numBatches].numVehicles > 0)
        schedule.numBatches++;

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
                int eid = batch.vehicleIds[i];
                int lA  = proposals.count(eid) ? proposals.at(eid).laneIndex   : 0;
                int tA  = proposals.count(eid) ? proposals.at(eid).intendedTurn : 0;
                if (movementsConflict(lA, tA, it->laneIndex, it->intendedTurn)) conflict = true;
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
