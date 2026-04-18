// RaftUtilities.cc — Shared helpers used by two or more protocol files.
//
// Covers: proposal building, TraCI wrappers, vehicle movement control,
//         fallback activation, intersection predicates, RAFT node management,
//         and metrics output.

#include "raft/RaftAppBase.h"
#include "raft/RaftLogger.h"
#include "veins/base/utils/FindModule.h"

#include <cstring>
#include <iostream>
#include <set>

#define NOW (simTime())

// ============ PROPOSAL VERIFICATION ============

// Verifies the cert and signature inside a SignedProposal received over the air.
// Returns true if the cert is signed by Emergency_CA and the payload signature is valid
// (i.e. this vehicle is a verified priority vehicle).
// Returns false for normal vehicles (Vehicle_CA) or if the cert/sig is invalid.
// Handles all logging internally.
bool RaftAppBase::verifySignedProposal(const SignedProposal& sp, int senderId) const
{
    std::string role = CryptoAuth::instance().verifyCert(sp.cert);
    bool sigOk = !role.empty() &&
                 CryptoAuth::instance().verifyProposalSignature(
                     sp.cert, sp.proposalBytes, sp.proposalSize,
                     sp.timestampMs, sp.signature, sp.signatureLen);

    if (!role.empty() && sigOk) {
        bool isPrio = (role == "priority");
        if (isPrio) {
            std::cout << NOW << " [CRYPTO][V" << myId_ << "] BEACON from V" << senderId
                      << " verified as PRIORITY (Emergency_CA)" << std::endl;
        }
        return isPrio;
    }
    if (!role.empty() || !sigOk) {
        std::cout << NOW << " [CRYPTO][V" << myId_ << "] BEACON from V" << senderId
                  << " cert/sig INVALID — treated as normal vehicle" << std::endl;
    }
    return false;
}


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
    myProposal_.isFirstInLane      = isLaneLeaderByTraci();
    myProposal_.blockedByVehicleId = myProposal_.isFirstInLane ? -1 : 0;
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
            auto leader = traciVehicle_->getLeader(intersectionStopDistance_ * (totalVehicles_ / 2.0));
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

// ============ SCHEDULE HELPERS ============

// Returns true if vehicleId appears in any batch of the locally committed schedule.
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
        isPriorityVehicle_,
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
        myBatch_,
        std::vector<int>(activeVehicles_.begin(), activeVehicles_.end()),
        clusterMode_
    );
}
