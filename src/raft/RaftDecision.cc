// RaftDecision.cc — Crossing-order scheduling algorithm
// This is the only file you need to edit to change the intersection pass policy.
// computePassOrder() is called by proposePassOrder() in RaftCore.cc.
// applyCommittedPassOrder() has moved to RaftCoordination.cc (execution, not algorithm).

#include "raft/RaftAppBase.h"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <set>
#include <vector>

#define NOW (simTime())

// ============ LANE / TURN HELPERS ============

bool RaftAppBase::movementsConflict(int laneA, int turnA, int laneB, int turnB)
{
    if (laneA == laneB) return true;
    bool opposing = (laneA + laneB == 2) || (laneA + laneB == 4);
    if (opposing && turnA == 0 && turnB == 0) return false;
    if (opposing && turnA == 1 && turnB == 1) return false;
    return true;
}

// ============ STATIC HELPERS ============

// Checks whether vehicle v conflicts with any vehicle already in the given batch.
static bool conflictsWithBatch(
    const VehicleProposal& v,
    const PassBatch& batch,
    const std::map<int, VehicleProposal>& proposals)
{
    for (int i = 0; i < batch.numVehicles; i++) {
        int eid = batch.vehicleIds[i];
        int lA  = proposals.count(eid) ? proposals.at(eid).laneIndex    : 0;
        int tA  = proposals.count(eid) ? proposals.at(eid).intendedTurn : 0;
        if (RaftAppBase::movementsConflict(lA, tA, v.laneIndex, v.intendedTurn))
            return true;
    }
    return false;
}

// Adds vehicle v to the current open batch, opening a new batch if there is a conflict.
static void addVehicleToBatch(
    const VehicleProposal& v,
    PassScheduleEntry& schedule,
    std::set<int>& scheduled,
    const std::map<int, VehicleProposal>& proposals)
{
    while (schedule.numBatches < 16) {
        PassBatch& b = schedule.batches[schedule.numBatches];
        if (!conflictsWithBatch(v, b, proposals) && b.numVehicles < 8) {
            b.vehicleIds[b.numVehicles++] = v.vehicleId;
            scheduled.insert(v.vehicleId);
            return;
        }
        if (b.numVehicles > 0) schedule.numBatches++;
    }
}

// Fills the current open batch with non-conflicting vehicles from normalPool.
// Removes scheduled vehicles from normalPool in place.
// Does NOT open a new batch — only tops up the batch that priority scheduling opened.
static void fillBatchWithNormals(
    std::vector<VehicleProposal>& normalPool,
    PassScheduleEntry& schedule,
    std::set<int>& scheduled,
    const std::map<int, VehicleProposal>& proposals)
{
    PassBatch& b = schedule.batches[schedule.numBatches];
    for (auto it = normalPool.begin(); it != normalPool.end() && b.numVehicles < 8; ) {
        if (!conflictsWithBatch(*it, b, proposals)) {
            b.vehicleIds[b.numVehicles++] = it->vehicleId;
            scheduled.insert(it->vehicleId);
            it = normalPool.erase(it);
        } else {
            ++it;
        }
    }
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
//             After each vehicle is placed into a batch, fill remaining slots
//             with compatible normal-lane vehicles (no conflict).
//             This continues until the priority vehicle itself is scheduled.
//   Step 3 — Schedule remaining vehicles using the normal fairness algorithm.
//   If no priority lanes exist, skip to Step 3 directly.

PassScheduleEntry RaftAppBase::computePassOrder(
    const std::map<int, VehicleProposal>& proposals,
    const std::set<int>& /*activeVehicles*/)
{
    PassScheduleEntry schedule;
    memset(&schedule, 0, sizeof(schedule));
    std::set<int> scheduled;

    // ---- Step 1: identify priority lanes ----
    std::set<int> priorityLanes;
    for (const auto& kv : proposals) {
        if (kv.second.isPriority)
            priorityLanes.insert(kv.second.laneIndex);
    }

    if (!priorityLanes.empty()) {
        std::cout << "[PRIORITY] Priority vehicle detected in lane(s): ";
        for (int l : priorityLanes) std::cout << l << " ";
        std::cout << std::endl;
    }

    // Build normal pool (non-priority-lane vehicles) sorted by fairness criteria.
    // This pool is used to top up each priority batch with compatible vehicles.
    std::vector<VehicleProposal> normalPool;
    for (const auto& kv : proposals) {
        if (!priorityLanes.count(kv.second.laneIndex))
            normalPool.push_back(kv.second);
    }
    std::sort(normalPool.begin(), normalPool.end(),
              [](const VehicleProposal& a, const VehicleProposal& b) {
                  if (a.isFirstInLane != b.isFirstInLane) return a.isFirstInLane > b.isFirstInLane;
                  double waitDiff = a.waitingTimeMs - b.waitingTimeMs;
                  if (std::abs(waitDiff) > 500.0) return waitDiff > 0;
                  if (a.laneIndex != b.laneIndex) return a.laneIndex < b.laneIndex;
                  return a.distanceToJunction < b.distanceToJunction;
              });

    // ---- Step 2: schedule priority lanes (round-robin between them) ----
    // For each priority lane: schedule vehicles closest-to-junction first
    // until the priority vehicle is scheduled. Then that lane is "done".

    std::map<int, std::vector<VehicleProposal>> laneVehicles;
    for (const auto& kv : proposals) {
        if (priorityLanes.count(kv.second.laneIndex))
            laneVehicles[kv.second.laneIndex].push_back(kv.second);
    }
    for (auto& kv : laneVehicles) {
        std::sort(kv.second.begin(), kv.second.end(),
                  [](const VehicleProposal& a, const VehicleProposal& b) {
                      return a.distanceToJunction < b.distanceToJunction;
                  });
    }

    std::set<int> activePriorityLanes = priorityLanes;
    std::vector<int> prioLaneOrder(priorityLanes.begin(), priorityLanes.end());

    bool progress = true;
    while (!activePriorityLanes.empty() && progress) {
        progress = false;
        for (int lane : prioLaneOrder) {
            if (!activePriorityLanes.count(lane)) continue;
            auto& lvec = laneVehicles[lane];
            for (auto it = lvec.begin(); it != lvec.end(); ) {
                if (scheduled.count(it->vehicleId)) { it = lvec.erase(it); continue; }
                bool wasAmb = it->isPriority;
                addVehicleToBatch(*it, schedule, scheduled, proposals);
                // Fill the batch opened for this priority vehicle with compatible normals.
                fillBatchWithNormals(normalPool, schedule, scheduled, proposals);
                it = lvec.erase(it);
                progress = true;
                if (wasAmb) {
                    activePriorityLanes.erase(lane);
                    std::cout << "[PRIORITY] Priority vehicle in lane " << lane
                              << " scheduled — lane priority released." << std::endl;
                }
                break; // one vehicle per lane per round-robin turn
            }
            if (lvec.empty()) activePriorityLanes.erase(lane);
        }
    }

    // ---- Step 3: normal fairness algorithm for remaining vehicles ----
    // Rebuild pool: remaining normals (from normalPool, minus those already pulled
    // into priority batches) plus any priority-lane vehicles after the priority vehicle.
    std::vector<VehicleProposal> pool;
    for (const auto& kv : proposals) {
        if (!scheduled.count(kv.second.vehicleId))
            pool.push_back(kv.second);
    }
    std::sort(pool.begin(), pool.end(),
              [](const VehicleProposal& a, const VehicleProposal& b) {
                  if (a.isFirstInLane != b.isFirstInLane) return a.isFirstInLane > b.isFirstInLane;
                  double waitDiff = a.waitingTimeMs - b.waitingTimeMs;
                  if (std::abs(waitDiff) > 500.0) return waitDiff > 0;
                  if (a.laneIndex != b.laneIndex) return a.laneIndex < b.laneIndex;
                  return a.distanceToJunction < b.distanceToJunction;
              });

    // Commit the last batch opened by Step 2 (if non-empty) before starting Step 3.
    if (schedule.numBatches < 16 && schedule.batches[schedule.numBatches].numVehicles > 0)
        schedule.numBatches++;

    while (!pool.empty() && schedule.numBatches < 16) {
        PassBatch& batch = schedule.batches[schedule.numBatches];

        auto primaryIt = pool.begin();

        VehicleProposal primary = *primaryIt;
        batch.vehicleIds[batch.numVehicles++] = primary.vehicleId;
        scheduled.insert(primary.vehicleId);
        pool.erase(primaryIt);

        for (auto it = pool.begin(); it != pool.end() && batch.numVehicles < 8; ) {
            if (!conflictsWithBatch(*it, batch, proposals)) {
                batch.vehicleIds[batch.numVehicles++] = it->vehicleId;
                scheduled.insert(it->vehicleId);
                it = pool.erase(it);
            } else {
                ++it;
            }
        }
        schedule.numBatches++;
    }

    return schedule;
}
