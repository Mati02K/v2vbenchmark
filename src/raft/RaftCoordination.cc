// RaftCoordination.cc — Follower-side coordination handlers
// When the leader sends requests, followers respond here.
// Also handles incoming vehicle-passed / vehicle-left notifications.

#include "raft/RaftAppBase.h"
#include "raft/RaftLogger.h"

#include <cstring>
#include <iostream>

#define NOW (simTime())

// ============ STATUS REQUEST / RESPONSE ============

void RaftAppBase::handleStatusRequest(int fromLeader)
{
    if (hasPassedIntersection_) return;
    sendStatusResponse(fromLeader);
}

void RaftAppBase::sendStatusResponse(int toLeader)
{
    VehicleProposal proposal = buildMyProposal();
    std::vector<uint8_t> data(sizeof(VehicleProposal));
    memcpy(data.data(), &proposal, sizeof(VehicleProposal));
    sendRaftToPeer(toLeader, /*COORD_STATUS_RESPONSE*/ 0x31, data);
    messagesSent_++;
}

void RaftAppBase::handleStatusResponse(int fromVehicle, bool wos)
{
    if (!isLeader_ || !waitingForStatus_) return;
    collectedWayOfSight_[fromVehicle] = wos;
    statusResponseCount_++;
    if (statusResponseCount_ >= (int)activeVehicles_.size()) {
        collectStatusAndDecide();
    }
}

void RaftAppBase::handleStatusResponseProposal(int fromVehicle, const VehicleProposal& proposal)
{
    std::cout << simTime() << " [DBG][V" << myId_ << "] STATUS_RESPONSE from V" << fromVehicle
              << " isLeader=" << isLeader_ << " waitingForStatus=" << waitingForStatus_
              << " count=" << statusResponseCount_ << "/" << (totalVehicles_-1)
              << " wos=" << proposal.isFirstInLane
              << " dist=" << proposal.distanceToJunction << "m" << std::endl;
    if (!isLeader_ || !waitingForStatus_) return;
    collectedProposals_[fromVehicle] = proposal;
    collectedWayOfSight_[fromVehicle] = proposal.isFirstInLane;
    statusResponseCount_++;
    std::cout << simTime() << " [DBG][V" << myId_ << "] STATUS collected " << statusResponseCount_
              << "/" << (totalVehicles_-1) << " (need " << (totalVehicles_-1) << ")" << std::endl;
    if (statusResponseCount_ >= totalVehicles_ - 1) {
        waitingForStatus_ = false;
        if (timeStatusRequestSent_ > SIMTIME_ZERO) {
            statusCollectionTimeMs_ += (NOW - timeStatusRequestSent_).dbl() * 1000.0;
        }
        std::cout << simTime() << " [DBG][V" << myId_ << "] ALL STATUS COLLECTED -> proposePassOrder()" << std::endl;
        proposePassOrder();
    }
}

void RaftAppBase::collectStatusAndDecide()
{
    if (!isLeader_ || hasPassedIntersection_ || hasCommittedOrder_) return;
    waitingForStatus_ = false;
    if (timeStatusRequestSent_ > SIMTIME_ZERO) {
        statusCollectionTimeMs_ += (NOW - timeStatusRequestSent_).dbl() * 1000.0;
    }

    RAFT_LOG_LEADER("collected status from " << collectedWayOfSight_.size() << " vehicles");
    proposeStatusReport();
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
                      << " passed -> advancing to stop position (resume car-following)" << std::endl;
            RAFT_LOG("front vehicle passed — advancing to stop position (chain)");
            try {
                traciVehicle_->setSpeedMode(31);  // restore SUMO car-following
                traciVehicle_->setSpeed(-1);      // let SUMO advance us
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
}
