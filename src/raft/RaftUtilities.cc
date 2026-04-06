// RaftUtilities.cc — Intersection detection, vehicle movement control, fallback, helpers

#include "raft/RaftAppBase.h"
#include "raft/RaftLogger.h"
#include "veins/base/utils/FindModule.h"

#include <cstring>
#include <iostream>

#define NOW (simTime())

// ============ PROPOSAL BUILDING ============

// Sets the static fields of myProposal_ once — called after vehicle identity,
// route, and crypto cert are fully initialised.
void RaftAppBase::initMyProposal()
{
    memset(&myProposal_, 0, sizeof(myProposal_));
    myProposal_.vehicleId  = myId_;
    myProposal_.isPriority = isPriorityVehicle_;
    strncpy(myProposal_.laneEdgeId, myLane_.c_str(), sizeof(myProposal_.laneEdgeId) - 1);
    myProposal_.intendedTurn = 0;  // STRAIGHT
    if (myRoute_.find("Left") != std::string::npos || myRoute_ == "rL")       myProposal_.intendedTurn = 1;
    else if (myRoute_.find("Right") != std::string::npos || myRoute_ == "rR") myProposal_.intendedTurn = 2;
}

// Refreshes only the dynamic fields of myProposal_ and returns a reference.
// Static fields (vehicleId, laneEdgeId, intendedTurn, isPriority) are untouched.
VehicleProposal RaftAppBase::updateMyProposal()
{
    calculateWayOfSight();  // refresh wayOfSight_ / vehicleInFrontOfMe_

    myProposal_.laneIndex          = myLaneIndex_;
    myProposal_.blockedByVehicleId = detectBlockingVehicle();
    myProposal_.isFirstInLane      = (myProposal_.blockedByVehicleId == -1);
    myProposal_.waitingTimeMs      = (timeStopped_ > SIMTIME_ZERO)
                                     ? (NOW - timeStopped_).dbl() * 1000.0 : 0.0;
    myProposal_.distanceToJunction = calculateDistanceToJunction();

    if (traciVehicle_) {
        try {
            myProposal_.positionOnLane = traciVehicle_->getLanePosition();
            myProposal_.speed          = traciVehicle_->getSpeed();
        } catch (...) {}
    }
    return myProposal_;
}

int RaftAppBase::detectBlockingVehicle()
{
    return vehicleInFrontOfMe_;
}

double RaftAppBase::calculateDistanceToJunction()
{
    return getDistanceToJunction();
}

// Ask TraCI if any vehicle is immediately ahead in the same lane.
// Returns true  → no vehicle ahead → I am the lane leader.
// Returns false → someone is ahead → I am queued behind them.
// Look-ahead distance = 200m (large enough to cover a full approach lane queue).
bool RaftAppBase::isLaneLeaderByTraci() const
{
    if (!traciVehicle_) return true;  // no TraCI yet → assume leader
    try {
        auto leader = traciVehicle_->getLeader(200.0);
        return leader.first.empty();  // empty string = nobody ahead in lane
    } catch (...) {
        return true;  // on error assume leader (safe default)
    }
}

// ============ INTERSECTION DETECTION ============

// Called once the first time a vehicle stops at the intersection.
// Triggers leader DB exchange (if lane leader) and the 10-second fallback timeout.
void RaftAppBase::onFirstStoppedAtIntersection()
{
    updateLaneLeaderFlag();

    std::cout << NOW << " [DBG][V" << myId_ << "] FIRST_STOP:"
              << " isLaneLeader=" << isLaneLeader_
              << " vehicleDB_=" << vehicleDB_.size()
              << " laneIdx=" << myLaneIndex_ << std::endl;

    if (isLaneLeader_) {
        sendLaneLeaderBeacon();
        scheduleClusterFormationLoop();
    }

    // Fallback: if RAFT cluster hasn't formed within fallbackClusterTimeoutMs_, activate fallback
    scheduleOneshotMs((double)fallbackClusterTimeoutMs_, [this]() {
        if (!raftStarted_ && !hasPassedIntersection_) {
            std::cout << NOW << " [WARN][V" << myId_ << "] CLUSTER_TIMEOUT (" << fallbackClusterTimeoutMs_
                      << "ms): RAFT not started. vehicleDB_=" << vehicleDB_.size()
                      << ". Activating fallback." << std::endl;
            coordinationMethod_ = "fallback";
            handleFallback();
        }
    });
}

// ---- scheduleLeaderStatusRequest -------------------------------------------
// Schedules sendStatusRequest after the cluster formation wait window, followed
// by a safety timeout that calls collectStatusAndDecide() if some followers
// never responded (packet loss / late arrival).
void RaftAppBase::scheduleLeaderStatusRequest()
{
    if (!isLeader_ || hasCommittedOrder_) return;
    double waitMs = intersectionStopTimeMs_;
    scheduleOneshotMs(waitMs, [this]() {
        if (isLeader_ && !hasCommittedOrder_ && !hasPassedIntersection_) {
            sendStatusRequest();
            scheduleOneshotMs(statusCollectionTimeoutMs_, [this]() {
                if (isLeader_ && !hasCommittedOrder_ && !hasPassedIntersection_)
                    collectStatusAndDecide();
            });
        }
    });
}

// ---- updateRoadTracking -----------------------------------------------------
// Detects road changes, logs them, and proactively caches the approach edge +
// lane index so cluster-junction detection can use them even if the approach
// edge was too short to trigger a stop check.
void RaftAppBase::updateRoadTracking(const std::string& roadId, const std::string& lastKnownRoad)
{
    if (roadId == lastKnownRoad) {
        // Periodic position debug (every 5s)
        if (NOW - lastStopDebugPrint_ > 5.0) {
            lastStopDebugPrint_ = NOW;
            double spd = 999.0; try { spd = traciVehicle_->getSpeed(); } catch (...) {}
            double d = getDistanceToJunction();
            std::cout << simTime() << " [DBG][V" << myId_ << "] POS roadId=" << roadId
                      << " dist=" << d << "m speed=" << spd << "m/s"
                      << " onApproach=" << (intersectionEdges_.count(roadId) ? "YES" : "NO")
                      << " stopped=" << hasStoppedAtIntersection_
                      << " phase=" << clusterPhase_
                      << " leader=" << isLeader_
                      << " committed=" << hasCommittedOrder_
                      << " myBatch=" << myBatch_
                      << " curBatch=" << currentBatch_ << std::endl;
        }
        return;
    }
    prevRoadId_ = roadId;
    double spd = 999.0; try { spd = traciVehicle_->getSpeed(); } catch (...) {}
    std::cout << simTime() << " [DBG][V" << myId_ << "] ROAD_CHANGE -> " << roadId
              << " dist=" << getDistanceToJunction() << "m speed=" << spd << "m/s"
              << " onApproach=" << (intersectionEdges_.count(roadId) ? "YES" : "NO") << std::endl;
    if (intersectionEdges_.count(roadId)) {
        intersectionEdge_ = roadId;
        for (int i = 0; i < (int)approachEdgeList_.size(); i++) {
            if (approachEdgeList_[i] == roadId) { myLaneIndex_ = i; break; }
        }
    }
}

// ---- handleClusterJunctionStop ----------------------------------------------
// Handles vehicles that entered the junction internal road without being caught
// on the approach edge (very short approach lane traversed between timer ticks).
void RaftAppBase::handleClusterJunctionStop(const std::string& roadId, const std::string& lastKnownRoad)
{
    if (hasStoppedAtIntersection_) return;

    // Recover approach edge from lastKnownRoad if not already cached.
    if ((intersectionEdge_.empty() || intersectionEdge_[0] == ':') &&
        !lastKnownRoad.empty() && lastKnownRoad[0] != ':') {
        for (int i = 0; i < (int)approachEdgeList_.size(); i++) {
            if (approachEdgeList_[i] == lastKnownRoad) {
                intersectionEdge_ = lastKnownRoad;
                myLaneIndex_ = i;
                break;
            }
        }
    }
    if (intersectionEdge_.empty()) intersectionEdge_ = roadId;

    stopVehicle();
    hasStoppedAtIntersection_ = true;
    timeStopped_ = NOW;
    calculateWayOfSight();
    std::cout << simTime() << " [DBG][V" << myId_ << "] STOPPED (cluster-junction)"
              << " road=" << roadId << " approachEdge=" << intersectionEdge_
              << " laneIdx=" << myLaneIndex_ << " wos=" << wayOfSight_
              << " isLeader=" << isLeader_ << std::endl;
    RAFT_LOG("STOPPED (cluster-junction) approachEdge=" << intersectionEdge_);
    onFirstStoppedAtIntersection();
    scheduleLeaderStatusRequest();
}

// ---- handleFrontStop --------------------------------------------------------
// Vehicle is at the front of the queue (within stop distance).
// First time: records stop, decides whether to go through or wait.
// Subsequent ticks: re-applies hard stop if still waiting.
void RaftAppBase::handleFrontStop(const std::string& roadId, double dist)
{
    if (!hasStoppedAtIntersection_) {
        hasStoppedAtIntersection_ = true;
        intersectionEdge_ = roadId;
        calculateWayOfSight();
        onFirstStoppedAtIntersection();

        bool alreadyMoving = (timeStartedMoving_ != SIMTIME_ZERO);
        bool canGoNow = !alreadyMoving && hasCommittedOrder_
                     && myBatch_ != -1 && myBatch_ <= currentBatch_
                     && !hasPassedIntersection_;

        if (alreadyMoving || canGoNow) {
            std::cout << simTime() << " [NO_STOP][V" << myId_ << "] batch=" << myBatch_
                      << " curBatch=" << currentBatch_
                      << " alreadyMoving=" << alreadyMoving << " -> skip stop" << std::endl;
            if (!alreadyMoving) resumeMovement();
        } else {
            timeStopped_ = NOW;
            stopVehicle();
            std::cout << simTime() << " [STOP_CHECK][V" << myId_ << "] STOPPED (front) dist="
                      << dist << "m batch=" << myBatch_ << " curBatch=" << currentBatch_
                      << " committed=" << hasCommittedOrder_
                      << " isLeader=" << isLeader_ << std::endl;
            RAFT_LOG("STOPPED (front) at dist=" << dist << "m");
            scheduleLeaderStatusRequest();
        }
    } else {
        // Re-apply hard stop if still waiting (prevents SUMO creep).
        if (!hasCommittedOrder_ || (myBatch_ > currentBatch_ && timeStartedMoving_ == SIMTIME_ZERO))
            stopVehicle();
    }
}

// ---- handleQueuedStop -------------------------------------------------------
// Vehicle is queued behind the front vehicle (further back, nearly stopped).
// SUMO car-following holds its position naturally — no hard stop needed.
void RaftAppBase::handleQueuedStop(const std::string& roadId, double dist, double speed)
{
    hasStoppedAtIntersection_ = true;
    timeStopped_     = NOW;
    intersectionEdge_ = roadId;
    calculateWayOfSight();
    std::cout << simTime() << " [STOP_CHECK][V" << myId_ << "] QUEUED (no hard stop) dist=" << dist
              << "m speed=" << speed << "m/s road=" << roadId
              << " wos=" << wayOfSight_ << " frontVeh=" << vehicleInFrontOfMe_
              << " isLeader=" << isLeader_ << std::endl;
    RAFT_LOG("QUEUED at dist=" << dist << "m speed=" << speed << "m/s");
    onFirstStoppedAtIntersection();
    scheduleLeaderStatusRequest();
}

// ---- checkAndStopAtIntersection ---------------------------------------------
// Called on every timer tick. Coordinates when/whether to stop at the junction.
void RaftAppBase::checkAndStopAtIntersection()
{
    if (!hasPassedIntersection_ && traciVehicle_) 
    {
        updateLaneLeaderFlag();
    }

    if (hasPassedIntersection_ || !traciVehicle_) return;
    if (timeStartedMoving_ > SIMTIME_ZERO) return;

    try {
        std::string roadId      = traciVehicle_->getRoadId();
        std::string lastKnownRoad = prevRoadId_;
        updateRoadTracking(roadId, lastKnownRoad);

        // Vehicle entered the junction internal road directly (short approach lane).
        bool onClusterJunction = (!roadId.empty() && roadId[0] == ':' &&
                                   roadId.find("cluster") != std::string::npos);
        if (onClusterJunction) {
            handleClusterJunctionStop(roadId, lastKnownRoad);
            return;
        }

        if (!intersectionEdges_.count(roadId)) return;

        double dist  = getDistanceToJunction();
        double speed = 999.0;
        try { speed = traciVehicle_->getSpeed(); } catch (...) {}

        bool closeEnough = (dist >= 0 && dist <= intersectionStopDistance_);
        bool queued      = (!closeEnough && speed < 1.5 &&
                            dist >= 0 && dist < intersectionStopDistance_ * 4.0 &&
                            !hasStoppedAtIntersection_);

        if (closeEnough) {
            handleFrontStop(roadId, dist);
        } else if (queued) {
            handleQueuedStop(roadId, dist, speed);
        }

    } catch (...) {}
}

// TraCI-based queue advancement: on every check-timer tick, ask SUMO who is ahead
// and how far. When the gap exceeds the stop distance (leader moved away), hand
// control back to SUMO car-following so the whole queue accordions forward.
// Once the vehicle reaches the stop line it is re-caught by checkAndStopAtIntersection.
void RaftAppBase::checkAndAdvanceInQueue()
{
    if (!hasStoppedAtIntersection_ || hasPassedIntersection_ || timeStartedMoving_ > SIMTIME_ZERO) return;
    if (!traciVehicle_) return;
    try {
        double speed = traciVehicle_->getSpeed();
        if (speed > 0.5) return;  // already moving under SUMO

        double dist = getDistanceToJunction();
        if (dist < 0 || dist <= intersectionStopDistance_) return;  // handled by checkAndStop

        // Ask TraCI for the leading vehicle and gap ahead
        auto leader = traciVehicle_->getLeader(intersectionStopDistance_ * 4.0);

        // Gap opened: no leader visible, or gap > 1× stop distance
        bool gapOpened = leader.first.empty() ||
                         leader.second > intersectionStopDistance_;

        if (NOW - lastQueueDebugPrint_ > 1.0) {
            lastQueueDebugPrint_ = NOW;
            std::cout << simTime() << " [QUEUE_ADV][V" << myId_ << "] dist=" << dist
                      << "m speed=" << speed
                      << " leader=" << (leader.first.empty() ? "NONE" : leader.first)
                      << " gap=" << leader.second << "m gapOpened=" << gapOpened
                      << " (need gap>" << intersectionStopDistance_ << "m)"
                      << " myBatch=" << myBatch_ << " curBatch=" << currentBatch_ << std::endl;
        }

        if (gapOpened) {
            std::cout << simTime() << " [QUEUE_ADV][V" << myId_ << "] RELEASING - front moved, taking place"
                      << " dist=" << dist << "m gap=" << leader.second << "m" << std::endl;
            RAFT_LOG("QUEUE ADVANCE (TraCI): gap=" << leader.second
                     << "m dist=" << dist << "m leader=" << leader.first);
            traciVehicle_->setSpeedMode(31);  // all SUMO safety checks on
            traciVehicle_->setSpeed(-1);       // SUMO car-following takes over
        }
    } catch (...) {}
}

bool RaftAppBase::isAtIntersection() const
{
    if (!traciVehicle_) return false;
    try {
        return intersectionEdges_.count(traciVehicle_->getRoadId()) > 0 && isNearJunction();
    } catch (...) { return false; }
}

bool RaftAppBase::isNearJunction() const
{
    double d = getDistanceToJunction();
    return d >= 0 && d <= intersectionStopDistance_;
}

bool RaftAppBase::hasPassedIntersectionEdge() const
{
    if (!traciVehicle_) return false;
    try {
        std::string roadId = traciVehicle_->getRoadId();
        if (exitEdges_.count(roadId)) {
            std::cout << simTime() << " Vehicle " << myId_ << " hasPassed=true (on exit edge " << roadId << ")" << std::endl;
            return true;
        }
        if (timeStartedMoving_ > SIMTIME_ZERO && !roadId.empty()) {
            bool onApproach  = intersectionEdges_.count(roadId) > 0;
            bool onInternal  = (roadId[0] == ':');
            if (!onApproach && !onInternal) {
                std::cout << simTime() << " Vehicle " << myId_ << " hasPassed=true (started moving, left approach/internal. roadId=" << roadId << ")" << std::endl;
                return true;
            }
        }
        return false;
    } catch (...) { return false; }
}

void RaftAppBase::calculateWayOfSight()
{
    // Step 1: determine actual laneIndex from the road this vehicle is currently on.
    if (traciVehicle_) {
        try {
            std::string roadId = traciVehicle_->getRoadId();
            // If on a junction internal edge, fall back to the cached approach edge for
            // lane index determination.
            const std::string& lookupRoad = (!roadId.empty() && roadId[0] == ':')
                                            ? intersectionEdge_ : roadId;
            for (int i = 0; i < (int)approachEdgeList_.size(); i++) {
                if (approachEdgeList_[i] == lookupRoad) {
                    myLaneIndex_ = i;
                    break;
                }
            }
        } catch (...) {}

        // Step 2: use TraCI leader to determine physical queue position.
        try {
            auto leader = traciVehicle_->getLeader(intersectionStopDistance_ * 4.0);
            if (leader.first.empty()) {
                wayOfSight_         = true;
                vehicleInFrontOfMe_ = -1;
            } else {
                wayOfSight_         = false;
                vehicleInFrontOfMe_ = -1;  // SUMO ID can't be directly mapped to OMNeT++ index
            }
            std::cout << simTime() << " [DBG][V" << myId_ << "] WOS_CALC"
                      << " laneIdx=" << myLaneIndex_
                      << " wos=" << wayOfSight_
                      << " traciLeader=" << (leader.first.empty() ? "NONE" : leader.first)
                      << " gap=" << leader.second << std::endl;
        } catch (...) {
            wayOfSight_         = true;
            vehicleInFrontOfMe_ = -1;
        }
        return;
    }

    // Fallback: no TraCI yet — use module-index formula (less accurate).
    int vehiclesPerSide = std::max(totalVehicles_ / 4, 1);
    int positionInLane  = myId_ % vehiclesPerSide;
    if (positionInLane == 0) {
        wayOfSight_         = true;
        vehicleInFrontOfMe_ = -1;
    } else {
        wayOfSight_         = true;
        vehicleInFrontOfMe_ = -1;
    }
}

// ============ VEHICLE MOVEMENT ============

void RaftAppBase::stopVehicle()
{
    if (!traciVehicle_) return;
    try {
        traciVehicle_->setSpeedMode(0);
        traciVehicle_->setSpeed(0);
    }
    catch (...) { RAFT_LOG_ERR("could not stop vehicle"); }
}

void RaftAppBase::resumeMovement()
{
    if (!traciVehicle_ || hasPassedIntersection_) return;
    std::cout << simTime() << " [DBG][V" << myId_ << "] RESUME_MOVEMENT called"
              << " hasStopped=" << hasStoppedAtIntersection_
              << " myBatch=" << myBatch_
              << " curBatch=" << currentBatch_
              << " isLeader=" << isLeader_
              << " fallback=" << isFallbackMode_ << std::endl;
    timeStartedMoving_ = NOW;
    RAFT_LOG("RESUMING movement");
    try {
        traciVehicle_->setSpeedMode(0);
        traciVehicle_->setParameter("jmIgnoreFoeProb",  "1.0");
        traciVehicle_->setParameter("jmIgnoreFoeSpeed", "100.0");
        traciVehicle_->setParameter("jmTimegapMinor",   "0.0");
        double speed = traciVehicle_->getMaxSpeed();
        if (speed <= 0) speed = 13.89;
        traciVehicle_->setSpeed(speed);
        timeStartedMoving_ = NOW;
    } catch (...) { RAFT_LOG_ERR("could not resume speed"); }
}

// ============ FALLBACK ============

void RaftAppBase::handleFallback()
{
    if (hasPassedIntersection_) return;
    isFallbackMode_     = true;
    coordinationMethod_ = "fallback";
    timeOrderCommitted_ = NOW;
    std::cout << simTime() << " [DBG][V" << myId_ << "] FALLBACK ACTIVATED"
              << " wos=" << wayOfSight_
              << " frontVeh=" << vehicleInFrontOfMe_
              << " failedElections=" << failedElectionCount_
              << " stopped=" << hasStoppedAtIntersection_
              << " committed=" << hasCommittedOrder_
              << " isLeader=" << isLeader_ << std::endl;
    RAFT_LOG("FALLBACK MODE activated");
    if (wayOfSight_ || vehicleInFrontOfMe_ == -1
        || !activeVehicles_.count(vehicleInFrontOfMe_)) {
        resumeMovement();
    } else {
        // Vehicle is queued behind another. Estimate queue position from lane layout
        // and stagger departure to avoid simultaneous movement into the intersection.
        int vehiclesPerSide = std::max(totalVehicles_ / 4, 1);
        int posInLane       = myId_ % vehiclesPerSide;
        int delayMs         = fallbackWaitMinMs_ + posInLane * 2000;
        RAFT_LOG("FALLBACK queued (pos=" << posInLane << ") — forced resume in " << delayMs << "ms");
        scheduleOneshotMs(delayMs, [this]() {
            if (!hasPassedIntersection_) resumeMovement();
        });
    }
}


// Returns true if vehicleId appears in any batch of the locally committed schedule.
// committedSchedule_ is populated when PASS_ORDER commits in RAFT — always reliable,
// unlike scheduledVehicles_ which is only filled by startNewRound() or QC_BROADCAST.
bool RaftAppBase::isVehicleInCommittedSchedule(int vehicleId) const
{
    for (int b = 0; b < committedSchedule_.numBatches; b++)
        for (int v = 0; v < committedSchedule_.batches[b].numVehicles; v++)
            if (committedSchedule_.batches[b].vehicleIds[v] == vehicleId)
                return true;
    return false;
}

// Returns true if any vehicle in vehicleDB_ is NOT in the committed schedule.
// Used by the multi-round trigger to decide whether a new RAFT round is needed.
bool RaftAppBase::hasUnscheduledVehicles() const
{
    for (auto& kv : vehicleDB_)
        if (!isVehicleInCommittedSchedule(kv.first))
            return true;
    return false;
}
