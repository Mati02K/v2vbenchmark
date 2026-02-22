// WillemtRaftWaveApplication.cc - RAFT over WAVE/802.11p transport
// Complete implementation with quorum consensus

#include "raft/WillemtRaftWaveApplication.h"
#include "raft/RaftWaveMessage_m.h"
#include <iostream>
#include <sstream>
#include <cstring>
#include <algorithm>
#include <cstddef>
#include <vector>
#include <iomanip>
#include <random>

extern "C" {
#include "../../third_party/raft/raft.h"
}

using namespace veins;

Define_Module(WillemtRaftWaveApplication);

// ============ STATIC MEMBERS ============

std::ofstream WillemtRaftWaveApplication::resultsFile_;
bool WillemtRaftWaveApplication::resultsFileOpened_ = false;
int WillemtRaftWaveApplication::vehiclesCompleted_ = 0;
int WillemtRaftWaveApplication::totalVehiclesStatic_ = 4;
std::string WillemtRaftWaveApplication::resultsFileNameStatic_ = "raft_results.json";
bool WillemtRaftWaveApplication::isGlobalInitialized_ = false;

// ============ CONSTRUCTOR / DESTRUCTOR ============

WillemtRaftWaveApplication::WillemtRaftWaveApplication()
    : clusterPhase_(PHASE_DISCOVERY)
    , raftServer_(nullptr)
    , myId_(-1)
    , myRaftNodeId_(-1)
    , myLaneIndex_(-1)
    , mobility_(nullptr)
    , traci_(nullptr)
    , traciVehicle_(nullptr)
    , discoveryTimer_(nullptr)
    , discoveryBeaconInterval_(0.3)
    , clusterTriggerDistance_(100.0)
    , clusterFormed_(false)
    , junctionX_(0)
    , junctionY_(0)
    , junctionPosKnown_(false)
    , isLeader_(false)
    , hasStoppedAtIntersection_(false)
    , hasPassedIntersection_(false)
    , wayOfSight_(true)
    , waitingForStatus_(false)
    , statusResponseCount_(0)
    , vehicleInFrontOfMe_(-1)
    , lastAppliedIndex_(0)
    , waitingForVehicle_(-1)
    , hasCommittedOrder_(false)
    , currentBatch_(0)
    , isFallbackMode_(false)
    , failedElectionCount_(0)
    , lastCheckedTerm_(0)
    , totalVehicles_(4)
    , electionTimeoutBaseMs_(150)
    , electionTimeoutJitterMs_(50)
    , requestTimeoutMs_(50)
    , maxFailedElections_(6)
    , fallbackWaitMinMs_(100)
    , fallbackWaitMaxMs_(300)
    , passConfirmationMs_(300)
    , statusCollectionTimeoutMs_(200)
    , checkTimer_(nullptr)
    , raftPeriodicTimer_(nullptr)
    , statusTimeoutTimer_(nullptr)
    , passOrderTimer_(nullptr)
    , fallbackTimer_(nullptr)
    , timeArrived_(0)
    , timeStopped_(0)
    , timeElected_(0)
    , timeOrderCommitted_(0)
    , timeStartedMoving_(0)
    , timePassed_(0)
    , messagesSent_(0)
    , messagesReceived_(0)
    , electionRounds_(0)
    , wasElectedLeader_(false)
    , coordinationMethod_("raft")
    , logEntriesProposed_(0)
    , logEntriesCommitted_(0)
    , metricsWritten_(false)
{
    memset(&committedPassOrder_, 0, sizeof(committedPassOrder_));
    memset(&committedSchedule_, 0, sizeof(committedSchedule_));
}

WillemtRaftWaveApplication::~WillemtRaftWaveApplication()
{
    if (raftServer_) {
        raft_free(raftServer_);
        raftServer_ = nullptr;
    }
}

// ============ FILE MANAGEMENT ============

void WillemtRaftWaveApplication::openResultsFile(const std::string& filename)
{
    if (!resultsFileOpened_) {
        resultsFile_.open(filename, std::ios::out | std::ios::trunc);
        if (resultsFile_.is_open()) {
            resultsFile_ << "[\n";
            resultsFileOpened_ = true;
            resultsFileNameStatic_ = filename;
            std::cout << "Opened results file: " << filename << std::endl;
        } else {
            std::cerr << "ERROR: Failed to open results file: " << filename << std::endl;
        }
    }
}

void WillemtRaftWaveApplication::closeResultsFile()
{
    if (resultsFileOpened_) {
        resultsFile_ << "\n]";
        resultsFile_.close();
        resultsFileOpened_ = false;
        vehiclesCompleted_ = 0;
        isGlobalInitialized_ = false;
    }
}

// ============ INITIALIZATION ============

// ============ EDGE PARAMETER PARSING ============

void WillemtRaftWaveApplication::parseEdgeParameters()
{
    // Parse approach edges (comma-separated)
    std::string approachStr = par("approachEdges").stdstringValue();
    std::string exitStr = par("exitEdges").stdstringValue();
    
    approachEdgeList_.clear();
    exitEdgeList_.clear();
    intersectionEdges_.clear();
    exitEdges_.clear();
    
    // Split comma-separated strings
    std::stringstream ssApproach(approachStr);
    std::string token;
    while (std::getline(ssApproach, token, ',')) {
        // Trim whitespace
        size_t start = token.find_first_not_of(" \t");
        size_t end = token.find_last_not_of(" \t");
        if (start != std::string::npos) {
            token = token.substr(start, end - start + 1);
            approachEdgeList_.push_back(token);
            intersectionEdges_.insert(token);
        }
    }
    
    std::stringstream ssExit(exitStr);
    while (std::getline(ssExit, token, ',')) {
        size_t start = token.find_first_not_of(" \t");
        size_t end = token.find_last_not_of(" \t");
        if (start != std::string::npos) {
            token = token.substr(start, end - start + 1);
            exitEdgeList_.push_back(token);
            exitEdges_.insert(token);
            intersectionEdges_.insert(token);
        }
    }
    
    std::cout << "Vehicle " << myId_ << " edge config: "
              << approachEdgeList_.size() << " approach edges, "
              << exitEdgeList_.size() << " exit edges" << std::endl;
}

// ============ INITIALIZATION (continued) ============

void WillemtRaftWaveApplication::initialize(int stage)
{
    DemoBaseApplLayer::initialize(stage);
    
    if (stage == 0) {
        myId_ = getParentModule()->getIndex();
        myRaftNodeId_ = myId_ + 1;
        
        // Read parameters from NED
        try {
            totalVehicles_ = par("totalVehicles").intValue();
            electionTimeoutBaseMs_ = par("electionTimeoutBaseMs").intValue();
            electionTimeoutJitterMs_ = par("electionTimeoutJitterMs").intValue();
            requestTimeoutMs_ = par("requestTimeoutMs").intValue();
            maxFailedElections_ = par("maxFailedElections").intValue();
            fallbackWaitMinMs_ = par("fallbackWaitMinMs").intValue();
            fallbackWaitMaxMs_ = par("fallbackWaitMaxMs").intValue();
            passConfirmationMs_ = par("passConfirmationMs").intValue();
            
            int baseTimeout = par("statusCollectionTimeoutMs").intValue();
            statusCollectionTimeoutMs_ = baseTimeout;
            
            resultsFileName_ = par("resultsFile").stdstringValue();
            
            // Dynamic cluster params
            clusterTriggerDistance_ = par("clusterTriggerDistance").doubleValue();
            discoveryBeaconInterval_ = par("discoveryBeaconInterval").doubleValue();
        } catch (std::exception& e) {
             std::cerr << "Vehicle " << myId_ << " ERROR reading params: " << e.what() << std::endl;
        }
        
        // Parse dynamic intersection edge configuration
        parseEdgeParameters();
        
        // Calculate vehicle's lane from static assignment (will be refined by TraCI)
        int vehiclesPerSide = totalVehicles_ / 4;
        if (vehiclesPerSide == 0) vehiclesPerSide = 1;
        
        int sideIndex = myId_ / vehiclesPerSide;
        myLaneIndex_ = sideIndex % 4;
        
        // Assign lane from dynamic approach edge list
        if (approachEdgeList_.empty()) {
             std::cerr << "Vehicle " << myId_ << " ERROR: approachEdgeList is empty!" << std::endl;
        } else {
             int dirIndex = sideIndex % (int)approachEdgeList_.size();
             myLane_ = approachEdgeList_[dirIndex];
        }

        // Route names for logging
        static const char* routeNames[] = {"rW", "rS", "rE", "rN"};
        myRoute_ = routeNames[sideIndex % 4];
        
        int positionInLane = myId_ % vehiclesPerSide;
        if (positionInLane > 0) {
            vehicleInFrontOfMe_ = myId_ - 1;
            wayOfSight_ = false;
        } else {
            vehicleInFrontOfMe_ = -1;
            wayOfSight_ = true;
        }
        
        // Initialize active vehicles (all vehicles)
        for (int i = 0; i < totalVehicles_; i++) {
            activeVehicles_.insert(i);
        }
        
        if (!isGlobalInitialized_) {
            isGlobalInitialized_ = true;
            totalVehiclesStatic_ = totalVehicles_;
            openResultsFile(resultsFileName_);
        }
        
        timeArrived_ = simTime();
        
        // Start in DISCOVERY phase — no RAFT server yet
        clusterPhase_ = PHASE_DISCOVERY;
        
        // Create timers
        checkTimer_ = new cMessage("checkTimer");
        raftPeriodicTimer_ = new cMessage("raftPeriodicTimer");
        discoveryTimer_ = new cMessage("discoveryTimer");
        
        // Schedule check timer (for intersection detection)
        scheduleAt(simTime() + CHECK_INTERVAL, checkTimer_);
        
        // Schedule discovery beacon timer
        scheduleAt(simTime() + uniform(0, discoveryBeaconInterval_), discoveryTimer_);
    }
}

void WillemtRaftWaveApplication::handlePositionUpdate(cObject* obj)
{
    DemoBaseApplLayer::handlePositionUpdate(obj);
    
    // Get mobility on first position update
    if (!mobility_) {
        mobility_ = TraCIMobilityAccess().get(getParentModule());
        if (mobility_) {
            traci_ = mobility_->getCommandInterface();
            traciVehicle_ = mobility_->getVehicleCommandInterface();
        }
    }
}

// ============ DISCOVERY PHASE ============

void WillemtRaftWaveApplication::sendDiscoveryBeacon()
{
    // Beacon includes vehicleId + clusterPhase so receivers can filter
    std::vector<uint8_t> data(sizeof(int) + sizeof(uint8_t));
    memcpy(data.data(), &myId_, sizeof(int));
    uint8_t phase = static_cast<uint8_t>(clusterPhase_);
    data[sizeof(int)] = phase;
    broadcastRaftMessage(benchmark::DISCOVERY_BEACON, data);
}

void WillemtRaftWaveApplication::handleDiscoveryBeacon(int senderId)
{
    if (senderId == myId_) return;
    // Only track peers that are still active and haven't left
    if (clusterPhase_ == PHASE_DISCOVERY && activeVehicles_.count(senderId) > 0) {
        discoveredPeers_.insert(senderId);
    }
}

double WillemtRaftWaveApplication::getDistanceToJunction() const
{
    if (!mobility_) return 999999.0;
    
    // Use the position from Veins mobility
    auto pos = mobility_->getPositionAt(simTime());
    
    if (!junctionPosKnown_) return 999999.0;
    
    double dx = pos.x - junctionX_;
    double dy = pos.y - junctionY_;
    return sqrt(dx * dx + dy * dy);
}

void WillemtRaftWaveApplication::checkClusterTrigger()
{
    if (clusterPhase_ != PHASE_DISCOVERY) return;
    if (!traciVehicle_) return;
    
    // Only trigger cluster formation after stopping at the intersection
    // This ensures all approaching vehicles have had time to exchange beacons
    if (!hasStoppedAtIntersection_) return;
    
    // Trigger if we've discovered at least one peer
    if (!discoveredPeers_.empty()) {
        std::cout << std::fixed << std::setprecision(1)
                  << (simTime().dbl() * 1000.0) << "ms Vehicle " << myId_
                  << " CLUSTER TRIGGER (stopped, discovered " << discoveredPeers_.size() << " peers)" << std::endl;
        broadcastClusterForm();
        return;
    }
    
    // Fallback: if stopped alone for > 3 seconds, form single-member cluster
    double waitTime = (simTime() - timeStopped_).dbl();
    if (timeStopped_ > SIMTIME_ZERO && waitTime > 3.0) {
        std::cout << std::fixed << std::setprecision(1)
                  << (simTime().dbl() * 1000.0) << "ms Vehicle " << myId_
                  << " CLUSTER TRIGGER (fallback, waited " << std::setprecision(1) << waitTime << "s alone)" << std::endl;
        broadcastClusterForm();
    }
}

void WillemtRaftWaveApplication::broadcastClusterForm()
{
    if (clusterFormed_) return;
    
    // Build member list = active discoveredPeers + self
    // Filter out vehicles that already left the intersection
    std::set<int> members;
    members.insert(myId_);
    for (int peerId : discoveredPeers_) {
        if (activeVehicles_.count(peerId) > 0) {
            members.insert(peerId);
        }
    }
    
    // Serialize: numMembers + memberIds[]
    int numMembers = (int)members.size();
    std::vector<uint8_t> data(sizeof(int) + numMembers * sizeof(int));
    memcpy(data.data(), &numMembers, sizeof(int));
    int offset = sizeof(int);
    for (int id : members) {
        memcpy(data.data() + offset, &id, sizeof(int));
        offset += sizeof(int);
    }
    
    broadcastRaftMessage(benchmark::CLUSTER_FORM, data);
    
    // Form cluster locally
    formCluster(members);
}

void WillemtRaftWaveApplication::handleClusterForm(const std::vector<uint8_t>& data, int senderId)
{
    if (clusterFormed_) return;  // Already formed
    
    // Deserialize member list
    if (data.size() < sizeof(int)) return;
    
    int numMembers;
    memcpy(&numMembers, data.data(), sizeof(int));
    
    std::set<int> members;
    int offset = sizeof(int);
    for (int i = 0; i < numMembers && offset + sizeof(int) <= data.size(); i++) {
        int memberId;
        memcpy(&memberId, data.data() + offset, sizeof(int));
        members.insert(memberId);
        offset += sizeof(int);
    }
    
    // Make sure we're in the list
    members.insert(myId_);
    
    std::cout << std::fixed << std::setprecision(1)
              << (simTime().dbl() * 1000.0) << "ms Vehicle " << myId_
              << " received CLUSTER_FORM from " << senderId
              << " with " << members.size() << " members" << std::endl;
    
    formCluster(members);
}

void WillemtRaftWaveApplication::formCluster(const std::set<int>& members)
{
    if (clusterFormed_) return;
    clusterFormed_ = true;
    clusterPhase_ = PHASE_COORDINATION;
    
    // Stop discovery beacons
    if (discoveryTimer_ && discoveryTimer_->isScheduled()) {
        cancelEvent(discoveryTimer_);
    }
    
    std::cout << std::fixed << std::setprecision(1)
              << (simTime().dbl() * 1000.0) << "ms Vehicle " << myId_
              << " FORMING CLUSTER with " << members.size() << " members: [";
    bool first = true;
    for (int id : members) {
        if (!first) std::cout << ",";
        std::cout << id;
        first = false;
    }
    std::cout << "]" << std::endl;
    
    // Create RAFT server now
    raftServer_ = raft_new();
    if (!raftServer_) {
        EV_ERROR << "Vehicle " << myId_ << " ERROR: Could not create RAFT server!" << endl;
        return;
    }
    
    // Set up RAFT callbacks
    raft_cbs_t callbacks;
    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.send_requestvote = sendRequestVote;
    callbacks.send_appendentries = sendAppendEntries;
    callbacks.log_offer = logOffer;
    callbacks.applylog = applylog;
    callbacks.persist_vote = persistVote;
    
    raft_set_callbacks(raftServer_, &callbacks, this);
    
    // Set timeouts with jitter
    int electionTimeout = electionTimeoutBaseMs_ + intuniform(0, electionTimeoutJitterMs_);
    raft_set_election_timeout(raftServer_, electionTimeout);
    raft_set_request_timeout(raftServer_, requestTimeoutMs_);
    
    // Add only discovered cluster members (dynamic!)
    for (int id : members) {
        int nodeId = id + 1;
        void* userData = reinterpret_cast<void*>(static_cast<intptr_t>(id));
        int isSelf = (id == myId_) ? 1 : 0;
        raft_add_node(raftServer_, userData, nodeId, isSelf);
    }
    
    // Start RAFT periodic timer
    if (!raftPeriodicTimer_->isScheduled()) {
        scheduleAt(simTime() + RAFT_PERIODIC_INTERVAL, raftPeriodicTimer_);
    }
}

// ============ FAIR SCHEDULING HELPERS ============

WillemtRaftWaveApplication::VehicleProposal WillemtRaftWaveApplication::buildMyProposal()
{
    VehicleProposal proposal;
    memset(&proposal, 0, sizeof(proposal));
    proposal.vehicleId = myId_;
    strncpy(proposal.laneEdgeId, myLane_.c_str(), sizeof(proposal.laneEdgeId) - 1);
    proposal.laneIndex = myLaneIndex_;
    proposal.intendedTurn = 0; // STRAIGHT (default for this benchmark)
    proposal.isFirstInLane = amIFirstInLane();
    
    // Get live position/speed from TraCI
    if (traciVehicle_) {
        try {
            proposal.positionOnLane = traciVehicle_->getLanePosition();
            proposal.speed = traciVehicle_->getSpeed();
        } catch (...) {
            proposal.positionOnLane = 0;
            proposal.speed = 0;
        }
    }
    
    return proposal;
}

bool WillemtRaftWaveApplication::amIFirstInLane()
{
    // Check if this vehicle is the frontmost (closest to junction) on its lane
    // In the current setup, vehicle with positionInLane==0 is first
    int vehiclesPerSide = totalVehicles_ / 4;
    if (vehiclesPerSide == 0) vehiclesPerSide = 1;
    int posInLane = myId_ % vehiclesPerSide;
    return (posInLane == 0);
}

bool WillemtRaftWaveApplication::movementsConflict(int laneA, int turnA, int laneB, int turnB)
{
    // Conflict matrix for intersection movements
    // Lanes: 0=W, 1=S, 2=E, 3=N
    // Turns: 0=STRAIGHT, 1=LEFT, 2=RIGHT
    
    if (laneA == laneB) return true; // Same direction always conflicts
    
    // Opposing straight movements don't conflict
    // W(0)↔E(2), N(3)↔S(1) are opposing
    bool opposing = (laneA + laneB == 2) || (laneA + laneB == 4);
    
    if (opposing && turnA == 0 && turnB == 0) return false; // Opposing straight = OK
    if (opposing && turnA == 1 && turnB == 1) return false; // Opposing left = OK
    
    // Right turns: right turn from lane X doesn't conflict with 
    // straight from the lane they're merging onto
    // For now, be conservative: all other combinations conflict
    return true;
}

bool WillemtRaftWaveApplication::conflictsWithBatch(const VehicleProposal& proposal, 
                                                    const std::vector<VehicleProposal>& batch)
{
    for (const auto& existing : batch) {
        if (movementsConflict(proposal.laneIndex, proposal.intendedTurn,
                              existing.laneIndex, existing.intendedTurn)) {
            return true;
        }
    }
    return false;
}

void WillemtRaftWaveApplication::finish()
{
    if (!metricsWritten_ && resultsFileOpened_ && hasStoppedAtIntersection_) {
        simtime_t now = simTime();
        
        if (timeStartedMoving_ == SIMTIME_ZERO) {
            timeStartedMoving_ = now;
        }
        if (timePassed_ == SIMTIME_ZERO) {
            timePassed_ = now;
        }
        
        outputMetricsJSON();
    }
    
    // Cleanup timers
    cancelAndDelete(checkTimer_);
    cancelAndDelete(raftPeriodicTimer_);
    if (discoveryTimer_) cancelAndDelete(discoveryTimer_);
    if (statusTimeoutTimer_) cancelAndDelete(statusTimeoutTimer_);
    if (passOrderTimer_) cancelAndDelete(passOrderTimer_);
    if (fallbackTimer_) cancelAndDelete(fallbackTimer_);
    
    if (raftServer_) {
        raft_free(raftServer_);
        raftServer_ = nullptr;
    }

    // Only close results file if all vehicles have completed
    // (otherwise, other vehicles still need to write their metrics)
    if (resultsFileOpened_ && vehiclesCompleted_ >= totalVehiclesStatic_) {
        closeResultsFile();
    }
    
    DemoBaseApplLayer::finish();
}

// ============ SELF MESSAGE HANDLING ============

void WillemtRaftWaveApplication::handleSelfMsg(cMessage* msg)
{
    if (msg == checkTimer_) {
        checkAndStopAtIntersection();
        if (hasStoppedAtIntersection_ && !hasPassedIntersection_) {
            checkIfLeftIntersection();
        }
        if (!hasPassedIntersection_) {
            scheduleAt(simTime() + CHECK_INTERVAL, checkTimer_);
        }
    }
    else if (msg == discoveryTimer_) {
        // Send beacons in DISCOVERY phase, and continue after cluster formation
        // so late-arriving vehicles can see our clustered status
        if (clusterPhase_ == PHASE_DISCOVERY) {
            sendDiscoveryBeacon();
            checkClusterTrigger();
            scheduleAt(simTime() + discoveryBeaconInterval_, discoveryTimer_);
        } else if (clusterPhase_ == PHASE_COORDINATION && !hasPassedIntersection_) {
            // Keep sending beacons so late vehicles don't add us
            sendDiscoveryBeacon();
            scheduleAt(simTime() + discoveryBeaconInterval_ * 2, discoveryTimer_);
        }
    }
    else if (msg == raftPeriodicTimer_) {
        processRaftPeriodic();
        if (!hasPassedIntersection_ && !isFallbackMode_) {
            scheduleAt(simTime() + RAFT_PERIODIC_INTERVAL, raftPeriodicTimer_);
        }
    }
    else if (msg == statusTimeoutTimer_) {
        // Status collection timeout - proceed with what we have
        if (isLeader_ && waitingForStatus_ && !hasCommittedOrder_) {
            waitingForStatus_ = false;
            proposePassOrder();
        }
        statusTimeoutTimer_ = nullptr;
    }
    else if (msg == passOrderTimer_) {
        if (isLeader_ && !hasPassedIntersection_ && !hasCommittedOrder_) {
            proposePassOrder();
        }
        delete msg;
        passOrderTimer_ = nullptr;
    }
    else if (strcmp(msg->getName(), "statusResponseTimer") == 0) {
        int leaderId = msg->getKind();
        int vehiclesPerSide = (totalVehicles_ > 0) ? totalVehicles_ / 4 : 1;
        if (vehiclesPerSide == 0) vehiclesPerSide = 1;
        int posInLane = myId_ % vehiclesPerSide;
        int dir = 0; // All traffic is Straight in this benchmark
        sendStatusResponse(leaderId, wayOfSight_, posInLane, dir);
        delete msg;
    }
    else if (strcmp(msg->getName(), "passOrderDelay") == 0) {
        resumeMovement();
        delete msg;
    }
    else if (msg == fallbackTimer_) {
        handleFallback();
        delete msg;
        fallbackTimer_ = nullptr;
    }
    else {
        DemoBaseApplLayer::handleSelfMsg(msg);
    }
}

// ============ RAFT PERIODIC PROCESSING ============

void WillemtRaftWaveApplication::processRaftPeriodic()
{
    // Stop RAFT processing if we're done (committed order) or passed or in fallback
    // This prevents election storms when the leader leaves the intersection but followers act still stuck
    // Keep RAFT running (sending heartbeats) even after commit/passing to maintain cluster stability
    if (!raftServer_ || isFallbackMode_) return;

    static std::map<int, simtime_t> lastTimeMap;
    simtime_t now = simTime();
    simtime_t delta = now - lastTimeMap[myId_];
    lastTimeMap[myId_] = now;

    int msecElapsed = static_cast<int>(delta.dbl() * 1000.0);
    if (msecElapsed > 0) {
        raft_periodic(raftServer_, msecElapsed);

        // Track failed elections
        raft_term_t currentTerm = raft_get_current_term(raftServer_);
        if (currentTerm > lastCheckedTerm_) {
            electionRounds_++;
            if (!raft_is_leader(raftServer_)) {
                failedElectionCount_++;
                if (failedElectionCount_ >= maxFailedElections_ && 
                    hasStoppedAtIntersection_ && 
                    !hasCommittedOrder_) {
                    isFallbackMode_ = true;
                    coordinationMethod_ = "fallback";
                    handleFallback();
                    return;
                }
            } else {
                failedElectionCount_ = 0;
            }
            lastCheckedTerm_ = currentTerm;
        }

        // Check leadership changes
        bool wasLeader = isLeader_;
        isLeader_ = (raft_is_leader(raftServer_) == 1);

        if (isLeader_ && !wasLeader) {
            onBecameLeader();
        } else if (!isLeader_ && wasLeader) {
            onLostLeadership();
        }
        
        // Timeout fallback
        if (hasStoppedAtIntersection_ && !hasCommittedOrder_ && !isFallbackMode_) {
            simtime_t waitTime = now - timeStopped_;
            double timeoutThreshold = (double)totalVehiclesStatic_;
            if (waitTime.dbl() > timeoutThreshold) {
                std::cout << std::fixed << std::setprecision(1)
                          << (simTime().dbl() * 1000.0) << "ms Vehicle " << myId_
                          << " TIMEOUT: No PASS_ORDER after " << (waitTime.dbl() * 1000.0) 
                          << "ms, triggering fallback" << std::endl;
                handleFallback();
                return;
            }
        }
    }
}

// ============ RAFT STATIC CALLBACKS ============

int WillemtRaftWaveApplication::sendRequestVote(raft_server_t* raft, void* user_data,
                                            raft_node_t* node, msg_requestvote_t* msg)
{
    WillemtRaftWaveApplication* app = static_cast<WillemtRaftWaveApplication*>(user_data);
    return app->doSendRequestVote(node, msg);
}

int WillemtRaftWaveApplication::sendAppendEntries(raft_server_t* raft, void* user_data,
                                              raft_node_t* node, msg_appendentries_t* msg)
{
    WillemtRaftWaveApplication* app = static_cast<WillemtRaftWaveApplication*>(user_data);
    return app->doSendAppendEntries(node, msg);
}

int WillemtRaftWaveApplication::logOffer(raft_server_t* raft, void* user_data,
                                     raft_entry_t* entry, raft_index_t entry_idx)
{
    WillemtRaftWaveApplication* app = static_cast<WillemtRaftWaveApplication*>(user_data);
    return app->doLogOffer(entry, entry_idx);
}

int WillemtRaftWaveApplication::applylog(raft_server_t* raft, void* user_data,
                                    raft_entry_t* entry, raft_index_t entry_idx)
{
    WillemtRaftWaveApplication* app = static_cast<WillemtRaftWaveApplication*>(user_data);
    return app->doApplyLog(entry, entry_idx);
}

void WillemtRaftWaveApplication::log(raft_server_t* raft, raft_node_t* node,
                                 void* user_data, const char* buf)
{
    // Optional logging
}

int WillemtRaftWaveApplication::persistVote(raft_server_t* raft, void* user_data, raft_node_id_t vote)
{
    return 0;
}

// ============ RAFT LOG HANDLING ============

int WillemtRaftWaveApplication::doLogOffer(raft_entry_t* entry, raft_index_t entry_idx)
{
    std::cout << std::fixed << std::setprecision(1)
              << (simTime().dbl() * 1000.0) << "ms Vehicle " << myId_
              << " doLogOffer called for entry #" << entry_idx 
              << " (len=" << entry->data.len << ")" << std::endl;
              
    if (entry->data.len < 1) {
        return -1;
    }

    uint8_t* data = static_cast<uint8_t*>(entry->data.buf);
    LogEntryType type = static_cast<LogEntryType>(data[0]);

    if (type == PASS_COMMAND && entry->data.len >= sizeof(PassCommandEntry) + 1) {
        std::cout << std::fixed << std::setprecision(1)
                  << (simTime().dbl() * 1000.0) << "ms Vehicle " << myId_
                  << " ACCEPTING log entry #" << entry_idx << " for replication" << std::endl;
        return 0;
    }

    return 0;
}

int WillemtRaftWaveApplication::doApplyLog(raft_entry_t* entry, raft_index_t entry_idx)
{
    std::cout << std::fixed << std::setprecision(1)
              << (simTime().dbl() * 1000.0) << "ms Vehicle " << myId_
              << " doApplyLog called for entry #" << entry_idx 
              << " (lastApplied=" << lastAppliedIndex_ << ", len=" << entry->data.len << ")" << std::endl;
              
    if (entry_idx <= lastAppliedIndex_) {
        return 0;
    }

    if (entry->data.len < 1) {
        return 0;
    }

    uint8_t* data = static_cast<uint8_t*>(entry->data.buf);
    LogEntryType type = static_cast<LogEntryType>(data[0]);
    
    std::cout << std::fixed << std::setprecision(1)
              << (simTime().dbl() * 1000.0) << "ms Vehicle " << myId_
              << " doApplyLog type=" << static_cast<int>(type) 
              << " (PASS_ORDER=" << static_cast<int>(PASS_ORDER) << ", PASS_COMMAND=" << static_cast<int>(PASS_COMMAND) << ")" << std::endl;

    // Handle STATUS_REPORT
    if (type == STATUS_REPORT && entry->data.len >= sizeof(StatusReportEntry) + 1) {
        StatusReportEntry* statusReport = reinterpret_cast<StatusReportEntry*>(data + 1);

        std::cout << std::fixed << std::setprecision(1)
                  << (simTime().dbl() * 1000.0) << "ms Vehicle " << myId_
                  << " APPLYING committed STATUS_REPORT entry #" << entry_idx
                  << " (" << statusReport->numVehicles << " vehicles)" << std::endl;

        committedStatuses_.clear();
        for (int i = 0; i < statusReport->numVehicles; i++) {
            VehicleStatus& status = statusReport->statuses[i];
            committedStatuses_[status.vehicleId] = status;
        }

        logEntriesCommitted_++;
        lastAppliedIndex_ = entry_idx;

        if (isLeader_ && !hasPassedIntersection_ && !hasCommittedOrder_) {
            if (!passOrderTimer_) {
                passOrderTimer_ = new cMessage("passOrderTimer");
                scheduleAt(simTime() + 0.05, passOrderTimer_);
            }
        }

        return 0;
    }

    // Handle PASS_ORDER
    if (type == PASS_ORDER && entry->data.len >= sizeof(PassOrderEntry) + 1) {
        PassOrderEntry* passOrder = reinterpret_cast<PassOrderEntry*>(data + 1);

        std::cout << std::fixed << std::setprecision(1)
                  << (simTime().dbl() * 1000.0) << "ms Vehicle " << myId_
                  << " APPLYING committed PASS_ORDER entry #" << entry_idx << " [";
        for (int i = 0; i < passOrder->numVehicles; i++) {
            std::cout << passOrder->order[i];
            if (i < passOrder->numVehicles - 1) std::cout << ",";
        }
        std::cout << "]" << std::endl;

        memcpy(&committedPassOrder_, passOrder, sizeof(PassOrderEntry));
        hasCommittedOrder_ = true;
        
        timeOrderCommitted_ = simTime();

        logEntriesCommitted_++;
        lastAppliedIndex_ = entry_idx;

        executePassOrder();

        return 0;
    }

    // Handle PASS_COMMAND (legacy)
    if (type == PASS_COMMAND && entry->data.len >= sizeof(PassCommandEntry) + 1) {
        PassCommandEntry* cmdData = reinterpret_cast<PassCommandEntry*>(data + 1);
        int vehicleId = cmdData->vehicleId;

        std::cout << std::fixed << std::setprecision(1)
                  << (simTime().dbl() * 1000.0) << "ms Vehicle " << myId_
                  << " APPLYING committed log entry #" << entry_idx
                  << " - PASS command for vehicle " << vehicleId << std::endl;

        logEntriesCommitted_++;
        lastAppliedIndex_ = entry_idx;

        executePassCommand(vehicleId);
        pendingProposals_.erase(entry_idx);
    }

    return 0;
}

// ============ WAVE MESSAGE SENDING ============

void WillemtRaftWaveApplication::sendRaftMessage(int msgType, int targetId, const std::vector<uint8_t>& data)
{
    benchmark::RaftWaveMessage* wsm = new benchmark::RaftWaveMessage();
    populateWSM(wsm);
    
    wsm->setMsgType(msgType);
    wsm->setSenderId(myId_);
    wsm->setTargetId(targetId);
    wsm->setPayloadLen(data.size());
    wsm->setPayloadArraySize(data.size());
    
    for (size_t i = 0; i < data.size(); i++) {
        wsm->setPayload(i, data[i]);
    }
    
    wsm->setRecipientAddress(-1);  // broadcast
    sendDown(wsm);
    
    messagesSent_++;
}

void WillemtRaftWaveApplication::broadcastRaftMessage(int msgType, const std::vector<uint8_t>& data)
{
    sendRaftMessage(msgType, -1, data);
}

int WillemtRaftWaveApplication::doSendRequestVote(raft_node_t* node, msg_requestvote_t* msg)
{
    if (!node) return -1;

    void* userData = raft_node_get_udata(node);
    int targetVehicleId = static_cast<int>(reinterpret_cast<intptr_t>(userData));

    if (activeVehicles_.find(targetVehicleId) == activeVehicles_.end()) {
        return 0;
    }

    std::vector<uint8_t> data = serializeRequestVote(msg);
    sendRaftMessage(benchmark::RAFT_REQUEST_VOTE, targetVehicleId, data);

    return 0;
}

int WillemtRaftWaveApplication::doSendAppendEntries(raft_node_t* node, msg_appendentries_t* msg)
{
    if (!node) return -1;

    void* userData = raft_node_get_udata(node);
    int targetVehicleId = static_cast<int>(reinterpret_cast<intptr_t>(userData));

    if (activeVehicles_.find(targetVehicleId) == activeVehicles_.end()) {
        return 0;
    }

    std::cout << std::fixed << std::setprecision(1)
              << (simTime().dbl() * 1000.0) << "ms Leader " << myId_
              << " SENDING AppendEntries to vehicle " << targetVehicleId
              << " (n_entries=" << msg->n_entries << ", leader_commit=" << msg->leader_commit << ")" << std::endl;

    std::vector<uint8_t> data = serializeAppendEntries(msg);
    sendRaftMessage(benchmark::RAFT_APPEND_ENTRIES, targetVehicleId, data);

    return 0;
}

// ============ WSM HANDLING ============

void WillemtRaftWaveApplication::onWSM(BaseFrame1609_4* frame)
{
    if (hasPassedIntersection_) return;

    benchmark::RaftWaveMessage* wsm = dynamic_cast<benchmark::RaftWaveMessage*>(frame);
    if (!wsm) {
        // Debug: Log when we receive a non-RAFT message
        EV_WARN << "Vehicle " << myId_ << " received non-RaftWaveMessage: " << frame->getClassName() << endl;
        return;
    }

    int msgType = wsm->getMsgType();
    int senderId = wsm->getSenderId();
    int targetId = wsm->getTargetId();

    // Check if message is for us (targetId == -1 for broadcast, or targetId == myId_ for unicast)
    if (targetId != -1 && targetId != myId_) {
        return;
    }

    // Extract payload
    std::vector<uint8_t> payload;
    unsigned int payloadLen = wsm->getPayloadLen();
    payload.resize(payloadLen);
    for (unsigned int i = 0; i < payloadLen; i++) {
        payload[i] = wsm->getPayload(i);
    }

    messagesReceived_++;

    switch (msgType) {
        // ---- Discovery Phase Messages ----
        case benchmark::DISCOVERY_BEACON:
        {
            // Extract cluster phase from beacon payload
            uint8_t senderPhase = 0;
            if (payload.size() >= sizeof(int) + sizeof(uint8_t)) {
                senderPhase = payload[sizeof(int)];
            }
            if (senderPhase == PHASE_DISCOVERY) {
                handleDiscoveryBeacon(senderId);
            } else {
                // Sender is already in a cluster — remove from our discoveredPeers
                discoveredPeers_.erase(senderId);
            }
            break;
        }
        case benchmark::CLUSTER_FORM:
            handleClusterForm(payload, senderId);
            break;
        
        // ---- RAFT Protocol Messages (require cluster to be formed) ----
        case benchmark::RAFT_REQUEST_VOTE:
            if (raftServer_) handleRequestVote(payload, senderId);
            break;
        case benchmark::RAFT_REQUEST_VOTE_RESPONSE:
            if (raftServer_) handleRequestVoteResponse(payload, senderId);
            break;
        case benchmark::RAFT_APPEND_ENTRIES:
            if (raftServer_) handleAppendEntries(payload, senderId);
            break;
        case benchmark::RAFT_APPEND_ENTRIES_RESPONSE:
            if (raftServer_) handleAppendEntriesResponse(payload, senderId);
            break;
        
        // ---- Coordination Messages ----
        case benchmark::COORD_STATUS_REQUEST:
            handleStatusRequest(senderId);
            break;
        case benchmark::COORD_STATUS_RESPONSE:
            if (payload.size() >= 3) {
                bool wos = (payload[0] == 1);
                int pos = static_cast<int>(payload[1]);
                int dir = static_cast<int>(payload[2]);
                handleStatusResponse(senderId, wos, pos, dir);
            }
            break;
        case benchmark::COORD_VEHICLE_PASSED:
            if (payload.size() >= 1) {
                int vehicleId = static_cast<int>(payload[0]);
                handleVehiclePassed(vehicleId);
            }
            break;
        case benchmark::COORD_VEHICLE_LEFT:
        case benchmark::COORD_VEHICLE_LEFT_REBROADCAST:
            if (payload.size() >= 1) {
                int vehicleId = static_cast<int>(payload[0]);
                handleVehicleLeft(vehicleId);
            }
            break;
    }
}

// ============ SERIALIZATION ============

std::vector<uint8_t> WillemtRaftWaveApplication::serializeRequestVote(msg_requestvote_t* msg)
{
    std::vector<uint8_t> data(sizeof(*msg));
    memcpy(data.data(), msg, sizeof(*msg));
    return data;
}

std::vector<uint8_t> WillemtRaftWaveApplication::serializeRequestVoteResponse(msg_requestvote_response_t* msg)
{
    std::vector<uint8_t> data(sizeof(*msg));
    memcpy(data.data(), msg, sizeof(*msg));
    return data;
}

std::vector<uint8_t> WillemtRaftWaveApplication::serializeAppendEntries(msg_appendentries_t* msg)
{
    size_t baseSize = offsetof(msg_appendentries_t, entries);
    std::vector<uint8_t> data(baseSize);
    memcpy(data.data(), msg, baseSize);
    
    // Note: n_entries is already in the base message via memcpy
    // Only serialize the entries array if there are entries
    if (msg->n_entries > 0 && msg->entries) {
        for (int i = 0; i < msg->n_entries; i++) {
            raft_entry_t* entry = &msg->entries[i];
            
            data.insert(data.end(), reinterpret_cast<uint8_t*>(&entry->term), 
                       reinterpret_cast<uint8_t*>(&entry->term) + sizeof(entry->term));
            data.insert(data.end(), reinterpret_cast<uint8_t*>(&entry->id), 
                       reinterpret_cast<uint8_t*>(&entry->id) + sizeof(entry->id));
            data.insert(data.end(), reinterpret_cast<uint8_t*>(&entry->type), 
                       reinterpret_cast<uint8_t*>(&entry->type) + sizeof(entry->type));
            
            uint32_t dataLen = entry->data.len;
            data.insert(data.end(), reinterpret_cast<uint8_t*>(&dataLen), 
                       reinterpret_cast<uint8_t*>(&dataLen) + sizeof(dataLen));
            
            if (dataLen > 0 && entry->data.buf) {
                uint8_t* buf = static_cast<uint8_t*>(entry->data.buf);
                data.insert(data.end(), buf, buf + dataLen);
            }
        }
    }
    
    return data;
}

std::vector<uint8_t> WillemtRaftWaveApplication::serializeAppendEntriesResponse(msg_appendentries_response_t* msg)
{
    std::vector<uint8_t> data(sizeof(*msg));
    memcpy(data.data(), msg, sizeof(*msg));
    return data;
}

void WillemtRaftWaveApplication::deserializeRequestVote(const std::vector<uint8_t>& data, msg_requestvote_t* msg)
{
    if (data.size() >= sizeof(*msg)) {
        memcpy(msg, data.data(), sizeof(*msg));
    }
}

void WillemtRaftWaveApplication::deserializeRequestVoteResponse(const std::vector<uint8_t>& data, msg_requestvote_response_t* msg)
{
    if (data.size() >= sizeof(*msg)) {
        memcpy(msg, data.data(), sizeof(*msg));
    }
}

void WillemtRaftWaveApplication::deserializeAppendEntries(const std::vector<uint8_t>& data, msg_appendentries_t* msg)
{
    size_t baseSize = offsetof(msg_appendentries_t, entries);
    if (data.size() < baseSize) {
        msg->entries = nullptr;
        msg->n_entries = 0;
        return;
    }
    
    memcpy(msg, data.data(), baseSize);
    
    // n_entries is already in the base message from memcpy
    // Only deserialize entries array if there are entries
    if (msg->n_entries > 0 && data.size() > baseSize) {
        size_t offset = baseSize;
        
        msg->entries = new raft_entry_t[msg->n_entries];
        
        for (int i = 0; i < msg->n_entries && offset < data.size(); i++) {
            raft_entry_t* entry = &msg->entries[i];
            
            if (offset + sizeof(entry->term) <= data.size()) {
                memcpy(&entry->term, &data[offset], sizeof(entry->term));
                offset += sizeof(entry->term);
            }
            if (offset + sizeof(entry->id) <= data.size()) {
                memcpy(&entry->id, &data[offset], sizeof(entry->id));
                offset += sizeof(entry->id);
            }
            if (offset + sizeof(entry->type) <= data.size()) {
                memcpy(&entry->type, &data[offset], sizeof(entry->type));
                offset += sizeof(entry->type);
            }
            
            uint32_t dataLen = 0;
            if (offset + sizeof(dataLen) <= data.size()) {
                memcpy(&dataLen, &data[offset], sizeof(dataLen));
                offset += sizeof(dataLen);
            }
            
            if (dataLen > 0 && offset + dataLen <= data.size()) {
                entry->data.len = dataLen;
                entry->data.buf = malloc(dataLen);
                memcpy(entry->data.buf, &data[offset], dataLen);
                
                // Debug: show first byte of entry data
                uint8_t firstByte = static_cast<uint8_t*>(entry->data.buf)[0];
                std::cout << "  Entry " << i << ": dataLen=" << dataLen 
                          << ", firstByte=" << static_cast<int>(firstByte) << std::endl;
                
                offset += dataLen;
            } else {
                entry->data.len = 0;
                entry->data.buf = nullptr;
            }
        }
    } else {
        msg->entries = nullptr;
    }
}

void WillemtRaftWaveApplication::deserializeAppendEntriesResponse(const std::vector<uint8_t>& data, msg_appendentries_response_t* msg)
{
    if (data.size() >= sizeof(*msg)) {
        memcpy(msg, data.data(), sizeof(*msg));
    }
}

// ============ RAFT MESSAGE HANDLERS ============

void WillemtRaftWaveApplication::handleRequestVote(const std::vector<uint8_t>& data, int senderId)
{
    msg_requestvote_t msg;
    deserializeRequestVote(data, &msg);
    
    msg_requestvote_response_t response;
    raft_recv_requestvote(raftServer_, raft_get_node(raftServer_, senderId + 1), &msg, &response);
    
    std::vector<uint8_t> respData = serializeRequestVoteResponse(&response);
    sendRaftMessage(benchmark::RAFT_REQUEST_VOTE_RESPONSE, senderId, respData);
}

void WillemtRaftWaveApplication::handleRequestVoteResponse(const std::vector<uint8_t>& data, int senderId)
{
    msg_requestvote_response_t msg;
    deserializeRequestVoteResponse(data, &msg);
    
    raft_recv_requestvote_response(raftServer_, raft_get_node(raftServer_, senderId + 1), &msg);
}

void WillemtRaftWaveApplication::handleAppendEntries(const std::vector<uint8_t>& data, int senderId)
{
    msg_appendentries_t msg;
    deserializeAppendEntries(data, &msg);
    
    std::cout << std::fixed << std::setprecision(1)
              << (simTime().dbl() * 1000.0) << "ms Vehicle " << myId_
              << " RECEIVED AppendEntries from " << senderId
              << " (n_entries=" << msg.n_entries << ", leader_commit=" << msg.leader_commit << ")" << std::endl;
    
    msg_appendentries_response_t response;
    raft_recv_appendentries(raftServer_, raft_get_node(raftServer_, senderId + 1), &msg, &response);
    
    std::cout << std::fixed << std::setprecision(1)
              << (simTime().dbl() * 1000.0) << "ms Vehicle " << myId_
              << " SENDING AppendEntriesResponse to " << senderId
              << " (success=" << response.success << ", current_idx=" << response.current_idx << ")" << std::endl;
    
    // Important: Do NOT free msg.entries[i].data.buf here!
    // The RAFT library takes ownership of these buffers and will use them when
    // applying log entries. Only free the entries array wrapper.
    if (msg.entries) {
        // Note: data.buf ownership is transferred to RAFT library
        // They will be freed when raft_free() is called or entries are popped
        delete[] msg.entries;
    }
    
    std::vector<uint8_t> respData = serializeAppendEntriesResponse(&response);
    sendRaftMessage(benchmark::RAFT_APPEND_ENTRIES_RESPONSE, senderId, respData);
}

void WillemtRaftWaveApplication::handleAppendEntriesResponse(const std::vector<uint8_t>& data, int senderId)
{
    msg_appendentries_response_t msg;
    deserializeAppendEntriesResponse(data, &msg);
    
    std::cout << std::fixed << std::setprecision(1)
              << (simTime().dbl() * 1000.0) << "ms Leader " << myId_
              << " RECEIVED AppendEntriesResponse from " << senderId
              << " (success=" << msg.success << ", current_idx=" << msg.current_idx << ")" << std::endl;
    
    raft_recv_appendentries_response(raftServer_, raft_get_node(raftServer_, senderId + 1), &msg);
}

// ============ COORDINATION PROTOCOL ============

void WillemtRaftWaveApplication::sendStatusRequest()
{
    std::vector<uint8_t> data;  // Empty payload
    broadcastRaftMessage(benchmark::COORD_STATUS_REQUEST, data);
    
    waitingForStatus_ = true;
    statusResponseCount_ = 0;
    collectedWayOfSight_.clear();
    collectedWayOfSight_[myId_] = wayOfSight_;
    
    // Start timeout
    if (statusTimeoutTimer_) {
        cancelEvent(statusTimeoutTimer_);
        delete statusTimeoutTimer_;
    }
    statusTimeoutTimer_ = new cMessage("statusTimeoutTimer");
    scheduleAt(simTime() + SimTime(statusCollectionTimeoutMs_, SIMTIME_MS), statusTimeoutTimer_);
}

// Delayed response with jitter to prevent collision storm
void WillemtRaftWaveApplication::handleStatusRequest(int fromLeader)
{
    cMessage* timer = new cMessage("statusResponseTimer");
    timer->setKind(fromLeader); 
    // Jitter 10ms to 50ms (faster collection for phase 2)
    scheduleAt(simTime() + uniform(0.01, 0.05), timer);
}

void WillemtRaftWaveApplication::sendStatusResponse(int toLeader, bool wayOfSight, int posInLane, int direction)
{
    std::vector<uint8_t> data(3);
    data[0] = wayOfSight ? 1 : 0;
    data[1] = static_cast<uint8_t>(posInLane);
    data[2] = static_cast<uint8_t>(direction);
    sendRaftMessage(benchmark::COORD_STATUS_RESPONSE, toLeader, data);
}

void WillemtRaftWaveApplication::handleStatusResponse(int fromVehicle, bool wayOfSight, int posInLane, int direction)
{
    if (!isLeader_ || !waitingForStatus_) return;
    
    collectedWayOfSight_[fromVehicle] = wayOfSight;
    committedStatuses_[fromVehicle].positionInLane = posInLane; // Store locally for calculation
    committedStatuses_[fromVehicle].direction = direction;
    statusResponseCount_++;
    
    if (statusResponseCount_ >= totalVehicles_ - 1) {
        waitingForStatus_ = false;
        if (statusTimeoutTimer_) {
            cancelEvent(statusTimeoutTimer_);
            delete statusTimeoutTimer_;
            statusTimeoutTimer_ = nullptr;
        }
        proposePassOrder();
    }
}

void WillemtRaftWaveApplication::sendVehiclePassed()
{
    std::vector<uint8_t> data(1);
    data[0] = static_cast<uint8_t>(myId_);
    broadcastRaftMessage(benchmark::COORD_VEHICLE_PASSED, data);
}

void WillemtRaftWaveApplication::handleVehiclePassed(int vehicleId)
{
    // Update wayOfSight if vehicle in front of us in same lane passed
    if (vehicleId == vehicleInFrontOfMe_) {
        wayOfSight_ = true;
    }
    
    // Resume movement if the vehicle we're waiting for in pass order has passed
    if (vehicleId == waitingForVehicle_ && hasCommittedOrder_ && !hasPassedIntersection_) {
        std::cout << std::fixed << std::setprecision(1)
                  << (simTime().dbl() * 1000.0) << "ms Vehicle " << myId_
                  << " vehicle " << vehicleId << " passed, resuming movement" << std::endl;
        waitingForVehicle_ = -1;
        resumeMovement();
    }
}

void WillemtRaftWaveApplication::sendVehicleLeft()
{
    std::vector<uint8_t> data(1);
    data[0] = static_cast<uint8_t>(myId_);
    broadcastRaftMessage(benchmark::COORD_VEHICLE_LEFT, data);
}

void WillemtRaftWaveApplication::handleVehicleLeft(int vehicleId)
{
    activeVehicles_.erase(vehicleId);
    
    if (vehicleId == waitingForVehicle_) {
        waitingForVehicle_ = -1;
        resumeMovement();
    }
    
    if (vehicleId == vehicleInFrontOfMe_) {
        vehicleInFrontOfMe_ = -1;
        wayOfSight_ = true;
    }
}

void WillemtRaftWaveApplication::rebroadcastVehicleLeft(int vehicleId)
{
    std::vector<uint8_t> data(1);
    data[0] = static_cast<uint8_t>(vehicleId);
    broadcastRaftMessage(benchmark::COORD_VEHICLE_LEFT_REBROADCAST, data);
}

// ============ PASS ORDER LOGIC ============

void WillemtRaftWaveApplication::proposeStatusReport()
{
    if (!isLeader_ || hasPassedIntersection_) return;
    
    StatusReportEntry report;
    memset(&report, 0, sizeof(report));
    report.numVehicles = collectedWayOfSight_.size();
    
    int idx = 0;
    for (const auto& kv : collectedWayOfSight_) {
        if (idx >= 32) break;
        report.statuses[idx].vehicleId = kv.first;
        report.statuses[idx].wayOfSight = kv.second;
        strncpy(report.statuses[idx].lane, myLane_.c_str(), 7);
        report.statuses[idx].positionInLane = 0;
        idx++;
    }
    
    // Create log entry
    size_t entrySize = 1 + sizeof(StatusReportEntry);
    uint8_t* entryData = new uint8_t[entrySize];
    entryData[0] = STATUS_REPORT;
    memcpy(entryData + 1, &report, sizeof(StatusReportEntry));
    
    raft_entry_t entry;
    memset(&entry, 0, sizeof(entry));
    entry.term = raft_get_current_term(raftServer_);
    entry.id = 0;
    entry.type = RAFT_LOGTYPE_NORMAL;
    entry.data.buf = entryData;
    entry.data.len = entrySize;
    
    msg_entry_response_t response;
    int result = raft_recv_entry(raftServer_, &entry, &response);
    
    if (result == 0) {
        logEntriesProposed_++;
        std::cout << std::fixed << std::setprecision(1)
                  << (simTime().dbl() * 1000.0) << "ms Leader " << myId_
                  << " PROPOSED STATUS_REPORT entry #" << response.idx << std::endl;
    }
    
    delete[] entryData;
}

void WillemtRaftWaveApplication::proposePassOrder()
{
    if (!isLeader_ || hasPassedIntersection_ || hasCommittedOrder_) return;
    
    // NEW: Use Conflict-Aware Parallel Scheduler
    std::vector<int> order = createDeterministicOrder(); // TODO: rename or replace body
    
    PassOrderEntry passOrder;
    memset(&passOrder, 0, sizeof(passOrder));
    passOrder.numVehicles = order.size();
    for (size_t i = 0; i < order.size() && i < 32; i++) {
        passOrder.order[i] = order[i];
    }
    
    size_t entrySize = 1 + sizeof(PassOrderEntry);
    uint8_t* entryData = new uint8_t[entrySize];
    entryData[0] = PASS_ORDER;
    memcpy(entryData + 1, &passOrder, sizeof(PassOrderEntry));
    
    raft_entry_t entry;
    memset(&entry, 0, sizeof(entry));
    entry.term = raft_get_current_term(raftServer_);
    entry.id = 0;
    entry.type = RAFT_LOGTYPE_NORMAL;
    entry.data.buf = entryData;
    entry.data.len = entrySize;
    
    msg_entry_response_t response;
    int result = raft_recv_entry(raftServer_, &entry, &response);
    
    if (result == 0) {
        logEntriesProposed_++;
        std::cout << std::fixed << std::setprecision(1)
                  << (simTime().dbl() * 1000.0) << "ms Leader " << myId_
                  << " PROPOSED PASS_ORDER entry #" << response.idx << " [";
        for (size_t i = 0; i < order.size(); i++) {
            std::cout << order[i];
            if (i < order.size() - 1) std::cout << ",";
        }
        std::cout << "]" << std::endl;
    } else {
        // Only delete if entry was rejected
        delete[] entryData;
    }
    // Note: Don't delete entryData on success - RAFT library owns the memory
}

std::vector<int> WillemtRaftWaveApplication::createDeterministicOrder()
{
    // PARALLEL SCHEDULER:
    // 1. Prioritize vehicles with WayOfSight=True (Safety) AND PosInLane=0 (Flow)
    // 2. Group non-conflicting directions (N+S, E+W)
    
    std::vector<int> order;
    std::vector<int> pool;
    for (int id : activeVehicles_) pool.push_back(id);
    
    // Safety check: if empty map, just use ID sort
    if (pool.empty()) return order;

    // Helper: Get Direction (0=Straight, 1=Left, 2=Right)
    auto getDir = [&](int id) { 
        if (committedStatuses_.find(id) != committedStatuses_.end()) 
            return committedStatuses_[id].direction;
        return 0; // Default
    };
    
    auto getLane = [&](int id) {
        int vps = (totalVehicles_ > 0) ? totalVehicles_ / 4 : 1;
        if (vps == 0) vps = 1;
        return (id / vps) % 4; // 0=N, 1=S, 2=E, 3=W
    };

    // Sort pool by priority: 
    // - Has Sight (True > False)
    // - Pos In Lane (0 > 1 > 2)
    // - Arrival Time (lower ID = earlier)
    std::sort(pool.begin(), pool.end(), [&](int a, int b) {
        bool wosA = collectedWayOfSight_[a];
        bool wosB = collectedWayOfSight_[b];
        if (wosA != wosB) return wosA > wosB;
        return a < b; // Fallback to ID
    });

    // Batching Logic (Strictly Sequential but grouped)
    // We stick to a linearized list for the LOG, but `executePassOrder` handles the speed.
    // Ideally, we put compatible vehicles adjacent.
    
    // Strategy: Take first available. Find compatible. Add them. Rinse repeat.
    while (!pool.empty()) {
        // Pick primary
        int primary = pool[0];
        order.push_back(primary);
        pool.erase(pool.begin());
        
        int laneA = getLane(primary);
        int dirA = getDir(primary);
        
        // Find compatible in remaining pool
        // Priority: Compatible Different Lane (Zipper) > Compatible Same Lane (Platoon)
        for (auto it = pool.begin(); it != pool.end(); ) {
            int candidate = *it;
            int laneB = getLane(candidate);
            
            bool compatible = false;
            bool sameLane = (laneA == laneB);
            
            // N(0)+S(1) or E(2)+W(3)
            if ((laneA == 0 && laneB == 1) || (laneA == 1 && laneB == 0)) compatible = true;
            if ((laneA == 2 && laneB == 3) || (laneA == 3 && laneB == 2)) compatible = true;
            
            // Enforce Zipper: If we haven't picked a cross-lane partner yet, skip same-lane for now
            // (Simplification: Just take any compatible. The previous logic took same-lane first because it was ID sorted)
            
            if (sameLane) compatible = true;

            if (compatible) {
                // To force zipper [0, 8, 1, 9]:
                // If sameLane is true, we only take it if we CANNOT find a different lane candidate?
                // Greedy approach:
                // Just add it?
                // Let's rely on standard greedy.
                // But previously ID sort put 1 before 8.
                // We need to prioritize 8 (Lane 1) over 1 (Lane 0).
                
                // Hack: Only accept Different Lane compatible in this pass?
                // Then second pass accept same lane?
                // Complexity is high.
                // Let's just output [0, 1, ... 8, 9] (Sequential Lanes) but ensure executePassOrder handles it.
                // BUT executePassOrder failed to maximize throughput.
                // So let's try to Force Interleave.
                
                order.push_back(candidate);
                it = pool.erase(it);
            } else {
                ++it;
            }
        }
    }
    
    return order;
}

void WillemtRaftWaveApplication::executePassOrder()
{
    if (hasPassedIntersection_) return;

    int myPosition = findMyPositionInOrder();
    
    if (myPosition < 0) {
        std::cout << std::fixed << std::setprecision(1)
                  << (simTime().dbl() * 1000.0) << "ms Vehicle " << myId_
                  << " NOT FOUND in pass order!" << std::endl;
        return;
    }

    std::cout << std::fixed << std::setprecision(1)
              << (simTime().dbl() * 1000.0) << "ms Vehicle " << myId_
              << " is at position " << myPosition << " in pass order" << std::endl;

    if (myPosition == 0) {
        // I'm first, start immediately
        std::cout << std::fixed << std::setprecision(1)
                  << (simTime().dbl() * 1000.0) << "ms Vehicle " << myId_
                  << " is FIRST in order, starting immediately" << std::endl;
        resumeMovement();
    } else {
        // Task 2: Parallel Phase Strategy
        int totalDelayMs = 0;
        auto hasConflict = [this](int otherVid) {
            int vPS = totalVehiclesStatic_ / 4;
            if (vPS == 0) vPS = 1;
            int otherSide = otherVid / vPS;
            int mySide = myId_ / vPS;
            
            // Non-conflicting: Opposing traffic going straight (W(0)&E(2) or S(1)&N(3))
            if ((mySide == 0 && otherSide == 2) || (mySide == 2 && otherSide == 0)) return false;
            if ((mySide == 1 && otherSide == 3) || (mySide == 3 && otherSide == 1)) return false;
            
            // All other combinations (crossing traffic, or same lane followers) must wait
            return true;
        };
        for (int i = 0; i < myPosition; i++) {
            if (hasConflict(committedPassOrder_.order[i])) totalDelayMs += 1000;
        }
        
        std::cout << std::fixed << std::setprecision(1)
                  << (simTime().dbl() * 1000.0) << "ms Vehicle " << myId_
                  << " at position " << myPosition << " (parallel: " << totalDelayMs << "ms delay)" << std::endl;
        
        // Schedule delay timer
        cMessage* delayTimer = new cMessage("passOrderDelay");
        scheduleAt(simTime() + SimTime(totalDelayMs, SIMTIME_MS), delayTimer);
    }
}

int WillemtRaftWaveApplication::findMyPositionInOrder()
{
    for (int i = 0; i < committedPassOrder_.numVehicles; i++) {
        if (committedPassOrder_.order[i] == myId_) {
            return i;
        }
    }
    return -1;
}

int WillemtRaftWaveApplication::getLaneIndex(const std::string& lane)
{
    if (lane == "N2C") return 0;
    if (lane == "S2C") return 1;
    if (lane == "E2C") return 2;
    if (lane == "W2C") return 3;
    return -1;
}

int WillemtRaftWaveApplication::selectVehicleToPass()
{
    for (const auto& kv : collectedWayOfSight_) {
        if (kv.second && activeVehicles_.count(kv.first)) {
            return kv.first;
        }
    }
    return -1;
}

void WillemtRaftWaveApplication::proposePassCommand(int vehicleId)
{
    // Legacy method - not used with PASS_ORDER
}

void WillemtRaftWaveApplication::executePassCommand(int vehicleId)
{
    if (vehicleId == myId_) {
        resumeMovement();
    }
}

void WillemtRaftWaveApplication::collectStatusAndDecide()
{
    sendStatusRequest();
}

void WillemtRaftWaveApplication::calculateWayOfSight()
{
    wayOfSight_ = (vehicleInFrontOfMe_ == -1 || 
                   activeVehicles_.find(vehicleInFrontOfMe_) == activeVehicles_.end());
}

// ============ INTERSECTION COORDINATION ============

void WillemtRaftWaveApplication::checkAndStopAtIntersection()
{
    if (hasStoppedAtIntersection_ || !traciVehicle_) return;

    try {
        if (isAtIntersection() && !hasStoppedAtIntersection_) {
            hasStoppedAtIntersection_ = true;
            timeStopped_ = simTime();
            intersectionEdge_ = traciVehicle_->getRoadId();
            calculateWayOfSight();
            
            // Only physically stop the vehicle if it hasn't already been given 
            // the green light to resume movement (e.g. joined cluster early)
            if (timeStartedMoving_ == SIMTIME_ZERO) {
                stopVehicle();
            }
        }
    } catch (...) {}
}

bool WillemtRaftWaveApplication::isAtIntersection() const
{
    if (!traciVehicle_) return false;
    
    try {
        std::string roadId = traciVehicle_->getRoadId();
        return intersectionEdges_.count(roadId) > 0;
    } catch (...) {
        return false;
    }
}

bool WillemtRaftWaveApplication::hasPassedIntersectionEdge() const
{
    if (!traciVehicle_) return false;
    
    try {
        std::string roadId = traciVehicle_->getRoadId();
        // Check if we're on an exit edge (primary check)
        if (exitEdges_.count(roadId) > 0) return true;
        
        // Robust fallback: if vehicle has resumed movement and is no longer
        // on its approach edge or an internal junction edge, it has passed.
        // Internal edges in SUMO start with ':'
        if (timeStartedMoving_ > SIMTIME_ZERO && !roadId.empty()) {
            bool onApproach = intersectionEdges_.count(roadId) > 0;
            bool onInternal = (roadId[0] == ':');
            if (!onApproach && !onInternal) {
                return true;  // On a different non-internal edge = has passed
            }
        }
        
        return false;
    } catch (...) {
        return false;
    }
}

void WillemtRaftWaveApplication::checkIfLeftIntersection()
{
    if (!hasPassedIntersection_ && hasPassedIntersectionEdge()) {
        hasPassedIntersection_ = true;
        timePassed_ = simTime();
        
        std::cout << std::fixed << std::setprecision(1)
                  << (simTime().dbl() * 1000.0) << "ms Vehicle " << myId_
                  << " LEFT intersection" << std::endl;
        
        // Notify waiting vehicles that we've actually passed (for pass order)
        sendVehiclePassed();
        sendVehicleLeft();
        outputMetricsJSON();
    }
}

void WillemtRaftWaveApplication::onBecameLeader()
{
    std::cout << std::fixed << std::setprecision(1)
              << (simTime().dbl() * 1000.0) << "ms Vehicle " << myId_
              << " BECAME LEADER (term=" << raft_get_current_term(raftServer_) << ")" << std::endl;
    
    wasElectedLeader_ = true;
    timeElected_ = simTime();
    failedElectionCount_ = 0;
    
    if (hasStoppedAtIntersection_ && !hasCommittedOrder_) {
        collectStatusAndDecide();
    }
}

void WillemtRaftWaveApplication::onLostLeadership()
{
    std::cout << std::fixed << std::setprecision(1)
              << (simTime().dbl() * 1000.0) << "ms Vehicle " << myId_
              << " LOST leadership" << std::endl;
    
    waitingForStatus_ = false;
    if (statusTimeoutTimer_) {
        cancelEvent(statusTimeoutTimer_);
        delete statusTimeoutTimer_;
        statusTimeoutTimer_ = nullptr;
    }
}

void WillemtRaftWaveApplication::resumeMovement()
{
    if (!traciVehicle_ || hasPassedIntersection_) return;
    
    timeStartedMoving_ = simTime();
    
    std::cout << std::fixed << std::setprecision(1)
              << (simTime().dbl() * 1000.0) << "ms Vehicle " << myId_
              << " RESUMING movement" << std::endl;
    
    try {
        // Use SpeedMode 23: Ignore right-of-way rules (bit 3) to prevent junction deadlocks,
        // but keep collision avoidance (bit 1) and car-following (bit 0 & 2) active.
        traciVehicle_->setSpeedMode(23);
        
        // Resume at target maximum speed
        double maxSpeed = traciVehicle_->getMaxSpeed();
        if (maxSpeed <= 0) maxSpeed = 13.89; // Default 50 km/h fallback
        traciVehicle_->setSpeed(maxSpeed);
        
        timeStartedMoving_ = simTime();
    } catch (...) {
        EV_WARN << "Vehicle " << myId_ << " could not resume speed" << endl;
    }
    
    // Note: sendVehiclePassed() is now called in checkIfLeftIntersection
    // when vehicle actually exits, enabling proper sequential coordination
}

void WillemtRaftWaveApplication::stopVehicle()
{
    if (!traciVehicle_) return;
    
    try {
        traciVehicle_->setSpeed(0);
    } catch (...) {
        EV_WARN << "Vehicle " << myId_ << " could not stop" << endl;
    }
}

void WillemtRaftWaveApplication::handleFallback()
{
    if (hasPassedIntersection_) return;
    
    isFallbackMode_ = true;
    coordinationMethod_ = "fallback";
    
    std::cout << std::fixed << std::setprecision(1)
              << (simTime().dbl() * 1000.0) << "ms Vehicle " << myId_
              << " FALLBACK MODE activated" << std::endl;
    
    // Cancel any existing fallback timer
    if (fallbackTimer_) {
        if (fallbackTimer_->isScheduled()) {
            cancelEvent(fallbackTimer_);
        }
        delete fallbackTimer_;
        fallbackTimer_ = nullptr;
    }
    
    // Mark as if we had consensus
    timeOrderCommitted_ = simTime();
    
    // Resume immediately if we have way of sight
    if (wayOfSight_ || vehicleInFrontOfMe_ == -1 || 
        activeVehicles_.find(vehicleInFrontOfMe_) == activeVehicles_.end()) {
        resumeMovement();
    }
}

// ============ METRICS OUTPUT ============

void WillemtRaftWaveApplication::outputMetricsJSON()
{
    if (metricsWritten_ || !resultsFileOpened_) return;
    metricsWritten_ = true;
    
    double stoppedMs = timeStopped_.dbl() * 1000.0;
    double electedMs = timeElected_.dbl() * 1000.0;
    double orderCommittedMs = timeOrderCommitted_.dbl() * 1000.0;
    double startedMovingMs = timeStartedMoving_.dbl() * 1000.0;
    double passedMs = timePassed_.dbl() * 1000.0;
    
    double totalWaitTime = passedMs - stoppedMs;
    double raftDecisionTime = (orderCommittedMs > 0 && stoppedMs > 0) ? (orderCommittedMs - stoppedMs) : 0;
    double transitTime = (passedMs > 0 && startedMovingMs > 0) ? (passedMs - startedMovingMs) : 0;
    double throughput = (totalWaitTime > 0) ? (1000.0 / totalWaitTime) : 0;
    
    if (vehiclesCompleted_ > 0) {
        resultsFile_ << ",\n";
    }
    
    resultsFile_ << "  {\n";
    resultsFile_ << "    \"vehicle_id\": " << myId_ << ",\n";
    resultsFile_ << "    \"lane\": \"" << myLane_ << "\",\n";
    resultsFile_ << "    \"was_leader\": " << (wasElectedLeader_ ? "true" : "false") << ",\n";
    resultsFile_ << "    \"coordination_method\": \"" << coordinationMethod_ << "\",\n";
    resultsFile_ << "    \"transport\": \"wave\",\n";
    resultsFile_ << "    \"timestamps_ms\": {\n";
    resultsFile_ << "      \"stopped\": " << std::fixed << std::setprecision(1) << stoppedMs << ",\n";
    resultsFile_ << "      \"elected\": " << electedMs << ",\n";
    resultsFile_ << "      \"order_committed\": " << orderCommittedMs << ",\n";
    resultsFile_ << "      \"started_moving\": " << startedMovingMs << ",\n";
    resultsFile_ << "      \"passed\": " << passedMs << "\n";
    resultsFile_ << "    },\n";
    resultsFile_ << "    \"durations_ms\": {\n";
    resultsFile_ << "      \"total_wait_time\": " << totalWaitTime << ",\n";
    resultsFile_ << "      \"raft_decision_time\": " << raftDecisionTime << ",\n";
    resultsFile_ << "      \"transit_time\": " << transitTime << "\n";
    resultsFile_ << "    },\n";
    resultsFile_ << "    \"throughput_veh_per_sec\": " << std::setprecision(4) << throughput << ",\n";
    resultsFile_ << "    \"messages\": {\n";
    resultsFile_ << "      \"sent\": " << messagesSent_ << ",\n";
    resultsFile_ << "      \"received\": " << messagesReceived_ << "\n";
    resultsFile_ << "    },\n";
    resultsFile_ << "    \"election_rounds\": " << electionRounds_ << ",\n";
    resultsFile_ << "    \"log_entries_proposed\": " << logEntriesProposed_ << ",\n";
    resultsFile_ << "    \"log_entries_committed\": " << logEntriesCommitted_ << "\n";
    resultsFile_ << "  }";
    
    resultsFile_.flush();
    
    vehiclesCompleted_++;
    
    std::cout << std::fixed << std::setprecision(1)
              << (simTime().dbl() * 1000.0) << "ms Vehicle " << myId_
              << " METRICS: wait=" << totalWaitTime << "ms, decision=" << raftDecisionTime 
              << "ms, method=" << coordinationMethod_ << std::endl;
    
    if (vehiclesCompleted_ >= totalVehiclesStatic_) {
        closeResultsFile();
        std::cout << "All " << totalVehiclesStatic_ << " vehicles completed. Terminating simulation." << std::endl;
        endSimulation();
    }
}



// ============ UTILITY FUNCTIONS ============

raft_node_id_t WillemtRaftWaveApplication::getNodeIdFromVehicleId(int vehicleId) const
{
    return vehicleId + 1;
}

int WillemtRaftWaveApplication::getVehicleIdFromNodeId(raft_node_id_t nodeId) const
{
    return nodeId - 1;
}
