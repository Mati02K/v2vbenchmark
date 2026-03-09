// RaftUtilities.cc — Intersection detection, vehicle movement control, fallback, gossip relay

#include "raft/RaftAppBase.h"
#include "raft/RaftLogger.h"
#include "veins/base/utils/FindModule.h"

#include <cstring>
#include <iostream>

#define NOW (simTime())

// ============ PROPOSAL BUILDING ============

VehicleProposal RaftAppBase::buildMyProposal()
{
    calculateWayOfSight();  // refresh isFirstInLane / vehicleInFrontOfMe_ before proposal

    VehicleProposal p;
    memset(&p, 0, sizeof(p));
    p.vehicleId = myId_;
    strncpy(p.laneEdgeId, myLane_.c_str(), sizeof(p.laneEdgeId) - 1);
    p.laneIndex           = myLaneIndex_;
    p.intendedTurn        = 0;  // Default STRAIGHT
    if (myRoute_.find("Left") != std::string::npos || myRoute_ == "rL") p.intendedTurn = 1;
    else if (myRoute_.find("Right") != std::string::npos || myRoute_ == "rR") p.intendedTurn = 2;
    p.blockedByVehicleId  = detectBlockingVehicle();
    p.isFirstInLane       = (p.blockedByVehicleId == -1);
    p.waitingTimeMs       = (timeStopped_ > SIMTIME_ZERO)
                            ? (NOW - timeStopped_).dbl() * 1000.0 : 0.0;
    p.distanceToJunction  = calculateDistanceToJunction();

    if (traciVehicle_) {
        try { p.positionOnLane = traciVehicle_->getLanePosition(); p.speed = traciVehicle_->getSpeed(); }
        catch (...) {}
    }
    return p;
}

int RaftAppBase::detectBlockingVehicle()
{
    return vehicleInFrontOfMe_;
}

double RaftAppBase::calculateDistanceToJunction()
{
    return getDistanceToJunction();
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
        sendLeaderDbExchange();
        scheduleLeaderDbExchangeLoop();
    }

    // Fallback: if RAFT cluster hasn't formed within fallbackClusterTimeoutMs_, activate fallback
    scheduleOneshotMs((double)fallbackClusterTimeoutMs_, [this]() {
        if (!raftStarted_ && !hasPassedIntersection_) {
            int numLanes = (int)approachEdgeList_.size();
            std::cout << NOW << " [WARN][V" << myId_ << "] CLUSTER_TIMEOUT (" << fallbackClusterTimeoutMs_ << "ms): received "
                      << receivedLeaderDBs_.size() << "/" << numLanes
                      << " leader DBs. Missing lanes: [";
            for (int i = 0; i < numLanes; i++)
                if (!receivedLeaderDBs_.count(i)) std::cout << i << " ";
            std::cout << "]. Activating fallback." << std::endl;
            coordinationMethod_ = "fallback";
            handleFallback();
        }
    });
}

void RaftAppBase::checkAndStopAtIntersection()
{
    // Update lane leader flag every check interval from vehicleDB_
    if (!hasPassedIntersection_ && traciVehicle_) updateLaneLeaderFlag();

    // Never re-trigger once the vehicle has been given pass permission or has passed.
    if (hasPassedIntersection_ || !traciVehicle_) return;
    if (timeStartedMoving_ > SIMTIME_ZERO) return;  // already cleared to go
    try {
        std::string roadId = traciVehicle_->getRoadId();

        // --- DBG: per-vehicle periodic position report + road transition ---
        bool roadChanged = (roadId != prevRoadId_);
        std::string lastKnownRoad = prevRoadId_;  // save before potential update
        if (roadChanged) {
            prevRoadId_ = roadId;
            double spd = 999.0;
            try { spd = traciVehicle_->getSpeed(); } catch (...) {}
            double d = getDistanceToJunction();
            std::cout << simTime() << " [DBG][V" << myId_ << "] ROAD_CHANGE -> " << roadId
                      << " dist=" << d << "m speed=" << spd << "m/s"
                      << " onApproach=" << (intersectionEdges_.count(roadId) ? "YES" : "NO") << std::endl;
            // Proactively cache approach edge + lane index so cluster-junction detection
            // can use them even if the approach edge was too short to trigger a stop.
            if (intersectionEdges_.count(roadId)) {
                intersectionEdge_ = roadId;
                for (int i = 0; i < (int)approachEdgeList_.size(); i++) {
                    if (approachEdgeList_[i] == roadId) {
                        myLaneIndex_ = i;
                        break;
                    }
                }
            }
        }
        if (NOW - lastStopDebugPrint_ > 5.0) {
            lastStopDebugPrint_ = NOW;
            double spd = 999.0;
            try { spd = traciVehicle_->getSpeed(); } catch (...) {}
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

        // Catch vehicles that enter the main intersection cluster junction without being
        // stopped on the approach edge (short approach lanes traversed in <CHECK_INTERVAL).
        bool onClusterJunction = (!roadId.empty() && roadId[0] == ':' &&
                                   roadId.find("cluster") != std::string::npos);
        if (onClusterJunction) {
            if (!hasStoppedAtIntersection_) {
                // If approach edge wasn't proactively cached, try lastKnownRoad as fallback.
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
                          << " road=" << roadId
                          << " approachEdge=" << intersectionEdge_
                          << " laneIdx=" << myLaneIndex_
                          << " wos=" << wayOfSight_
                          << " isLeader=" << isLeader_ << std::endl;
                RAFT_LOG("STOPPED (cluster-junction) approachEdge=" << intersectionEdge_);
                onFirstStoppedAtIntersection();
                if (isLeader_ && !hasCommittedOrder_) {
                    double waitMs = discoveryWaitMs_ + clusterFormationDelayMs_;
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
            }
            return;  // Do not proceed to approach-edge logic
        }

        if (!intersectionEdges_.count(roadId)) return;

        double dist  = getDistanceToJunction();
        double speed = 999.0;
        try { speed = traciVehicle_->getSpeed(); } catch (...) {}

        // Log raw distance/junction data for stop-position debugging
        double lanePos = 0, laneLen = 0;
        try {
            if (traci_) {
                std::string lid = traciVehicle_->getLaneId();
                lanePos = traciVehicle_->getLanePosition();
                laneLen = traci_->lane(lid).getLength();
            }
        } catch (...) {}
        if (dist >= 0 && dist <= intersectionStopDistance_ * 5.0 && !hasStoppedAtIntersection_) {
            std::cout << simTime() << " [STOP_CHECK][V" << myId_ << "] road=" << roadId
                      << " dist=" << dist << "m (lanePos=" << lanePos << " laneLen=" << laneLen << ")"
                      << " stopDist=" << intersectionStopDistance_ << "m speed=" << speed << "m/s"
                      << std::endl;
        }

        // Front of queue: within stop distance → hard stop and wait for pass order.
        bool closeEnough = (dist >= 0 && dist <= intersectionStopDistance_);
        // Queued behind front: slow/stopped further back.  Only record first detection.
        bool queued = (!closeEnough && speed < 1.5 &&
                       dist >= 0 && dist < intersectionStopDistance_ * 4.0 &&
                       !hasStoppedAtIntersection_);

        if (closeEnough) {
            // Always re-apply the hard stop (vehicle may have advanced in queue to here).
            stopVehicle();
            if (!hasStoppedAtIntersection_) {
                hasStoppedAtIntersection_ = true;
                timeStopped_     = NOW;
                intersectionEdge_ = roadId;
                calculateWayOfSight();
                std::cout << simTime() << " [STOP_CHECK][V" << myId_ << "] STOPPED (front, hard stop) dist="
                          << dist << "m road=" << roadId
                          << " wos=" << wayOfSight_
                          << " frontVeh=" << vehicleInFrontOfMe_
                          << " isLeader=" << isLeader_ << std::endl;
                RAFT_LOG("STOPPED (front) at dist=" << dist << "m");
                onFirstStoppedAtIntersection();
                if (isLeader_ && !hasCommittedOrder_) {
                    double waitMs = discoveryWaitMs_ + clusterFormationDelayMs_;
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
            }
        } else if (queued) {
            // Queued vehicle: record for metrics, no hard stop.
            // SUMO car-following holds its queue position naturally.
            // checkAndAdvanceInQueue() will release it via TraCI gap detection.
            hasStoppedAtIntersection_ = true;
            timeStopped_     = NOW;
            intersectionEdge_ = roadId;
            calculateWayOfSight();
            std::cout << simTime() << " [STOP_CHECK][V" << myId_ << "] QUEUED (no hard stop) dist=" << dist
                      << "m speed=" << speed << "m/s road=" << roadId
                      << " wos=" << wayOfSight_
                      << " frontVeh=" << vehicleInFrontOfMe_
                      << " isLeader=" << isLeader_ << std::endl;
            RAFT_LOG("QUEUED at dist=" << dist << "m speed=" << speed << "m/s");
            onFirstStoppedAtIntersection();
            if (isLeader_ && !hasCommittedOrder_) {
                double waitMs = discoveryWaitMs_ + clusterFormationDelayMs_;
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

