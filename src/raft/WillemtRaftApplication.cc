// WillemtRaftApplication.cc - COMPLETE IMPLEMENTATION WITH QUORUM CONSENSUS
// Part 1: Includes, statics, constructor, initialization

#include "raft/WillemtRaftApplication.h"
#include <iostream>
#include <sstream>
#include <cstring>
#include <algorithm>
#include <cstddef>
#include <vector>
#include <iomanip>
#include <random>
#include "inet/common/packet/Packet.h"
#include "inet/common/packet/chunk/BytesChunk.h"

extern "C" {
#include "../../third_party/raft/raft.h"
}

using namespace inet;

Define_Module(WillemtRaftApplication);

// ============ STATIC MEMBERS ============

std::ofstream WillemtRaftApplication::resultsFile_;
bool WillemtRaftApplication::resultsFileOpened_ = false;
int WillemtRaftApplication::vehiclesCompleted_ = 0;
int WillemtRaftApplication::totalVehiclesStatic_ = 4;
std::string WillemtRaftApplication::resultsFileNameStatic_ = "raft_results.json";

// ============ CONSTRUCTOR / DESTRUCTOR ============

WillemtRaftApplication::WillemtRaftApplication()
    : clusterPhase_(PHASE_DISCOVERY)
    , raftServer_(nullptr)
    , myId_(-1)
    , myRaftNodeId_(-1)
    , mobility_(nullptr)
    , traci_(nullptr)
    , traciVehicle_(nullptr)
    , discoveryBeaconInterval_(0.3)
    , clusterTriggerDistance_(100.0)
    , clusterFormed_(false)
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
    , myBatch_(-1)
    , myLaneIndex_(-1)
    , isFallbackMode_(false)
    , failedElectionCount_(0)
    , lastCheckedTerm_(0)
    , totalVehicles_(4)
    , electionTimeoutBaseMs_(150)
    , electionTimeoutJitterMs_(50)
    , requestTimeoutMs_(50)
    , intersectionStopDistance_(25.0)  // Stop within 25m of junction
    , arrivalWaitTimeMs_(3000)         // Wait 3s for vehicles to gather
    , maxFailedElections_(6)
    , fallbackWaitMinMs_(100)
    , fallbackWaitMaxMs_(300)
    , passConfirmationMs_(300)
    , statusCollectionTimeoutMs_(200)
    , timeArrived_(0)
    , timeStopped_(0)
    , timeClusterFormed_(SIMTIME_ZERO)
    , timeElected_(0)
    , timeOrderCommitted_(0)
    , timeStartedMoving_(0)
    , timePassed_(0)
    , waitingForVehiclesToArrive_(false)
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

WillemtRaftApplication::~WillemtRaftApplication()
{
    if (raftServer_) {
        raft_free(raftServer_);
        raftServer_ = nullptr;
    }
}

// ============ FILE MANAGEMENT ============

void WillemtRaftApplication::openResultsFile(const std::string& filename)
{
    if (!resultsFileOpened_) {
        resultsFile_.open(filename);
        resultsFile_ << "[\n";
        resultsFileOpened_ = true;
        resultsFileNameStatic_ = filename;
    }
}

void WillemtRaftApplication::closeResultsFile()
{
    if (resultsFileOpened_) {
        resultsFile_ << "\n]";
        resultsFile_.close();
        resultsFileOpened_ = false;
        vehiclesCompleted_ = 0;
    }
}

// ============ EDGE PARAMETER PARSING ============

void WillemtRaftApplication::parseEdgeParameters()
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

// ============ APPLICATION START ============

bool WillemtRaftApplication::startApplication()
{
    std::cout << "Vehicle  RAFT starting application." << std::endl;
    myId_ = getParentModule()->getIndex();
    myRaftNodeId_ = myId_ + 1;
    
    // Read parameters from NED
    totalVehicles_ = par("totalVehicles").intValue();
    electionTimeoutBaseMs_ = par("electionTimeoutBaseMs").intValue();
    electionTimeoutJitterMs_ = par("electionTimeoutJitterMs").intValue();
    requestTimeoutMs_ = par("requestTimeoutMs").intValue();
    maxFailedElections_ = par("maxFailedElections").intValue();
    fallbackWaitMinMs_ = par("fallbackWaitMinMs").intValue();
    fallbackWaitMaxMs_ = par("fallbackWaitMaxMs").intValue();
    passConfirmationMs_ = par("passConfirmationMs").intValue();
    
    // Scale status collection timeout with vehicle count for realistic timing
    // Base 200ms + 12.5ms per vehicle
    int baseTimeout = par("statusCollectionTimeoutMs").intValue();
    statusCollectionTimeoutMs_ = baseTimeout + (totalVehicles_ * 12.5);
    
    // Distance-based intersection stopping
    intersectionStopDistance_ = par("intersectionStopDistance").doubleValue();
    arrivalWaitTimeMs_ = par("arrivalWaitTimeMs").intValue();
    
    // Dynamic cluster formation parameters (matching WAVE)
    clusterTriggerDistance_ = par("clusterTriggerDistance").doubleValue();
    discoveryBeaconInterval_ = par("discoveryBeaconInterval").doubleValue();
    
    resultsFileName_ = par("resultsFile").stdstringValue();
    
    // Parse dynamic intersection edge configuration
    parseEdgeParameters();
    
    // Calculate vehicle's lane
    int vehiclesPerSide = totalVehicles_ / 4;
    if (vehiclesPerSide == 0) vehiclesPerSide = 1;
    
    int sideIndex = myId_ / vehiclesPerSide;
    int positionInLane = myId_ % vehiclesPerSide;
    
    // Assign lane from dynamic approach edge list
    int dirIndex = sideIndex % (int)approachEdgeList_.size();
    myLane_ = approachEdgeList_[dirIndex];
    // Route names for logging: rDir0, rDir1, rDir2, rDir3
    static const char* routeNames[] = {"rW", "rS", "rE", "rN"};
    myRoute_ = routeNames[dirIndex % 4];
    
    // Determine vehicle in front
    if (positionInLane > 0) {
        vehicleInFrontOfMe_ = myId_ - 1;
        wayOfSight_ = false;
    } else {
        vehicleInFrontOfMe_ = -1;
        wayOfSight_ = true;
    }
    
    // Initialize active vehicles
    for (int i = 0; i < totalVehicles_; i++) {
        activeVehicles_.insert(i);
    }
    
    totalVehiclesStatic_ = totalVehicles_;
    
    if (myId_ == 0) {
        openResultsFile(resultsFileName_);
    }
    
    // Get mobility
    mobility_ = check_and_cast<veins::VeinsInetMobility*>(getParentModule()->getSubmodule("mobility"));
    if (!mobility_) {
        EV_ERROR << "Vehicle " << myId_ << " ERROR: Could not find mobility module!" << endl;
        return false;
    }
    
    traci_ = mobility_->getCommandInterface();
    traciVehicle_ = mobility_->getVehicleCommandInterface();
    
    if (!traci_ || !traciVehicle_) {
        EV_ERROR << "Vehicle " << myId_ << " ERROR: Could not get TraCI interface!" << endl;
        return false;
    }
    
    timeArrived_ = simTime();
    
    // Determine lane index for scheduling (reuse vehiclesPerSide from above)
    myLaneIndex_ = (myId_ / vehiclesPerSide) % 4;
    
    // Start in DISCOVERY phase — no RAFT server yet (matching WAVE)
    clusterPhase_ = PHASE_DISCOVERY;
    
    // Schedule periodic checks
    timerManager.create(
        veins::TimerSpecification([this]() {
            checkAndStopAtIntersection();
            if (hasStoppedAtIntersection_ && !hasPassedIntersection_) {
                checkIfLeftIntersection();
            }
        }).interval(SimTime(CHECK_INTERVAL))
    );
    
    // Schedule discovery beacon timer (matching WAVE)
    timerManager.create(
        veins::TimerSpecification([this]() {
            if (clusterPhase_ == PHASE_DISCOVERY) {
                sendDiscoveryBeacon();
                checkClusterTrigger();
            } else if (clusterPhase_ == PHASE_COORDINATION && !hasPassedIntersection_) {
                // Continue beaconing so late joiners can discover and join
                sendDiscoveryBeacon();
                broadcastClusterExists();
            }
        }).interval(SimTime(discoveryBeaconInterval_))
    );
    
    // Schedule RAFT periodic timer (only active after cluster forms)
    timerManager.create(
        veins::TimerSpecification([this]() {
            if (raftServer_ && !hasPassedIntersection_ && !isFallbackMode_) {
                processRaftPeriodic();
            }
        }).interval(SimTime(RAFT_PERIODIC_INTERVAL))
    );
    
    return true;
}

bool WillemtRaftApplication::stopApplication()
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

    if (raftServer_) {
        raft_free(raftServer_);
        raftServer_ = nullptr;
    }
    return true;
}

// ============ DYNAMIC CLUSTER DISCOVERY (matching WAVE) ============

void WillemtRaftApplication::sendDiscoveryBeacon()
{
    std::ostringstream packetName;
    packetName << "discovery-beacon-from-" << myId_;
    
    // Payload: [vehicleId (4 bytes)][clusterPhase (1 byte)]
    std::vector<uint8_t> data(sizeof(int) + 1);
    memcpy(data.data(), &myId_, sizeof(int));
    data[sizeof(int)] = static_cast<uint8_t>(clusterPhase_);
    
    auto payload = makeShared<BytesChunk>(data);
    auto packet = createPacket(packetName.str());
    packet->insertAtBack(payload);
    sendPacket(std::move(packet));
    
    messagesSent_++;
}

void WillemtRaftApplication::handleDiscoveryBeacon(int senderId, uint8_t senderPhase)
{
    if (senderId == myId_) return;
    
    // Only track peers that are still active and haven't left
    if (clusterPhase_ == PHASE_DISCOVERY && activeVehicles_.count(senderId) > 0) {
        discoveredPeers_.insert(senderId);
    }
}

void WillemtRaftApplication::checkClusterTrigger()
{
    if (clusterPhase_ != PHASE_DISCOVERY) return;
    if (!traciVehicle_) return;
    
    // Trigger cluster formation when approaching intersection AND discovered peers
    bool atIntersection = false;
    try {
        std::string roadId = traciVehicle_->getRoadId();
        atIntersection = (intersectionEdges_.count(roadId) > 0);
    } catch (...) {}
    
    bool shouldTrigger = false;
    std::string triggerReason;
    
    if (atIntersection && !discoveredPeers_.empty()) {
        shouldTrigger = true;
        triggerReason = "at intersection with " + std::to_string(discoveredPeers_.size()) + " peers";
    } else if (hasStoppedAtIntersection_ && !discoveredPeers_.empty()) {
        shouldTrigger = true;
        triggerReason = "stopped with " + std::to_string(discoveredPeers_.size()) + " peers";
    } else if (hasStoppedAtIntersection_) {
        double waitTime = (simTime() - timeStopped_).dbl();
        if (waitTime > 2.0) {  // Fallback after 2 seconds alone
            shouldTrigger = true;
            triggerReason = "stopped alone for " + std::to_string(waitTime) + "s";
        }
    }
    
    if (shouldTrigger) {
        std::cout << std::fixed << std::setprecision(1)
                  << (simTime().dbl() * 1000.0) << "ms Vehicle " << myId_
                  << " CLUSTER TRIGGER (" << triggerReason << ")" << std::endl;
        broadcastClusterForm();
    }
}

void WillemtRaftApplication::broadcastClusterForm()
{
    if (clusterPhase_ != PHASE_DISCOVERY) return;
    
    clusterPhase_ = PHASE_FORMATION;
    
    // Collect all members: ourselves + discovered peers + all active vehicles
    std::set<int> allMembers;
    allMembers.insert(myId_);
    for (int peer : discoveredPeers_) {
        if (activeVehicles_.count(peer) > 0) {
            allMembers.insert(peer);
        }
    }
    // Add all other active vehicles for complete cluster
    for (int id : activeVehicles_) {
        allMembers.insert(id);
    }
    
    // Broadcast CLUSTER_FORM message
    std::ostringstream packetName;
    packetName << "cluster-form-from-" << myId_;
    
    // Payload: [numMembers (4 bytes)][memberIds... (4 bytes each)]
    std::vector<uint8_t> data;
    int numMembers = allMembers.size();
    data.resize(sizeof(int) + numMembers * sizeof(int));
    memcpy(data.data(), &numMembers, sizeof(int));
    int offset = sizeof(int);
    for (int id : allMembers) {
        memcpy(data.data() + offset, &id, sizeof(int));
        offset += sizeof(int);
    }
    
    auto payload = makeShared<BytesChunk>(data);
    auto packet = createPacket(packetName.str());
    packet->insertAtBack(payload);
    sendPacket(std::move(packet));
    
    messagesSent_++;
    
    // Form cluster locally
    formCluster(allMembers);
}

void WillemtRaftApplication::handleClusterForm(const std::vector<uint8_t>& data, int senderId)
{
    if (data.size() < sizeof(int)) return;
    
    // Parse members
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
    
    members.insert(myId_);
    
    if (!clusterFormed_) {
        std::cout << std::fixed << std::setprecision(1)
                  << (simTime().dbl() * 1000.0) << "ms Vehicle " << myId_
                  << " received CLUSTER_FORM from " << senderId
                  << " with " << members.size() << " members" << std::endl;
        formCluster(members);
    } else {
        // Already in cluster - merge late joiners
        mergeIntoCluster(members);
    }
}

void WillemtRaftApplication::handleClusterExists(const std::vector<uint8_t>& data, int senderId)
{
    // Format: [numMembers][memberIds...][clusterSize]
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
    
    members.insert(myId_);
    
    if (!clusterFormed_) {
        // Join the existing cluster
        std::cout << std::fixed << std::setprecision(1)
                  << (simTime().dbl() * 1000.0) << "ms Vehicle " << myId_
                  << " joining EXISTING cluster from " << senderId
                  << " with " << members.size() << " members" << std::endl;
        formCluster(members);
    } else if (raftServer_) {
        // Already in a cluster - check if we need to merge
        int ourClusterSize = raft_get_num_nodes(raftServer_);
        int theirClusterSize = numMembers;
        
        if (theirClusterSize > ourClusterSize) {
            std::cout << std::fixed << std::setprecision(1)
                      << (simTime().dbl() * 1000.0) << "ms Vehicle " << myId_
                      << " MERGING into larger cluster (ours=" << ourClusterSize 
                      << ", theirs=" << theirClusterSize << ")" << std::endl;
            mergeIntoCluster(members);
        }
    }
}

void WillemtRaftApplication::mergeIntoCluster(const std::set<int>& members)
{
    if (!raftServer_) return;
    
    for (int id : members) {
        int nodeId = id + 1;
        if (activeVehicles_.find(id) != activeVehicles_.end() && raft_get_node(raftServer_, nodeId) == nullptr) {
            raft_add_node(raftServer_, reinterpret_cast<void*>(static_cast<intptr_t>(id)), nodeId, id == myId_);
            
            std::cout << std::fixed << std::setprecision(1)
                      << (simTime().dbl() * 1000.0) << "ms Vehicle " << myId_
                      << " dynamically merged vehicle " << id << " into RAFT cluster" << std::endl;
        }
    }
}

void WillemtRaftApplication::broadcastClusterExists()
{
    if (!clusterFormed_ || !raftServer_) return;
    
    // Broadcast CLUSTER_EXISTS so late joiners can discover and join
    std::ostringstream packetName;
    packetName << "cluster-exists-from-" << myId_;
    
    // Build list of current cluster members
    std::vector<int> memberList;
    for (int id : activeVehicles_) {
        if (raft_get_node(raftServer_, id + 1) != nullptr) {
            memberList.push_back(id);
        }
    }
    
    // Payload: [numMembers][memberIds...]
    std::vector<uint8_t> data;
    int numMembers = memberList.size();
    data.resize(sizeof(int) + numMembers * sizeof(int));
    memcpy(data.data(), &numMembers, sizeof(int));
    int offset = sizeof(int);
    for (int id : memberList) {
        memcpy(data.data() + offset, &id, sizeof(int));
        offset += sizeof(int);
    }
    
    auto payload = makeShared<BytesChunk>(data);
    auto packet = createPacket(packetName.str());
    packet->insertAtBack(payload);
    sendPacket(std::move(packet));
    
    messagesSent_++;
}

void WillemtRaftApplication::formCluster(const std::set<int>& members)
{
    if (clusterFormed_) return;
    
    clusterPhase_ = PHASE_COORDINATION;
    clusterFormed_ = true;
    
    // Ensure myId_ is included
    std::set<int> allMembers = members;
    allMembers.insert(myId_);
    
    // Add all active vehicles to ensure consistent cluster membership
    for (int id : activeVehicles_) {
        allMembers.insert(id);
    }
    
    std::cout << std::fixed << std::setprecision(1)
              << (simTime().dbl() * 1000.0) << "ms Vehicle " << myId_
              << " FORMING CLUSTER with " << allMembers.size() << " members: [";
    bool first = true;
    for (int id : allMembers) {
        if (!first) std::cout << ",";
        std::cout << id;
        first = false;
    }
    std::cout << "]" << std::endl;
    
    // Create RAFT server
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
    
    // Add ALL active vehicles to ensure consistent cluster membership
    for (int id : allMembers) {
        int nodeId = id + 1;
        void* userData = reinterpret_cast<void*>(static_cast<intptr_t>(id));
        int isSelf = (id == myId_) ? 1 : 0;
        raft_add_node(raftServer_, userData, nodeId, isSelf);
    }
    
    // Track cluster formation time
    timeClusterFormed_ = simTime();
}

// Continue to Part 2...

// WillemtRaftApplication.cc Part 2 - RAFT CALLBACKS AND PERIODIC

// ============ RAFT PERIODIC PROCESSING ============

void WillemtRaftApplication::processRaftPeriodic()
{
    if (!raftServer_ || hasPassedIntersection_ || isFallbackMode_) return;

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
                // Only trigger fallback if we don't have a committed order
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
        
        // TIMEOUT-BASED FALLBACK: If we've been waiting too long without PASS_ORDER
        // Use a longer timeout to account for vehicles arriving at different times
        if (hasStoppedAtIntersection_ && !hasCommittedOrder_ && !isFallbackMode_) {
            simtime_t waitTime = now - timeStopped_;
            // Timeout: base 15s + 0.5s per vehicle (accounts for staggered arrivals)
            double timeoutThreshold = 15.0 + (double)totalVehiclesStatic_ * 0.5;
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

int WillemtRaftApplication::sendRequestVote(raft_server_t* raft, void* user_data,
                                            raft_node_t* node, msg_requestvote_t* msg)
{
    WillemtRaftApplication* app = static_cast<WillemtRaftApplication*>(user_data);
    return app->doSendRequestVote(node, msg);
}

int WillemtRaftApplication::sendAppendEntries(raft_server_t* raft, void* user_data,
                                              raft_node_t* node, msg_appendentries_t* msg)
{
    WillemtRaftApplication* app = static_cast<WillemtRaftApplication*>(user_data);
    return app->doSendAppendEntries(node, msg);
}

int WillemtRaftApplication::logOffer(raft_server_t* raft, void* user_data,
                                     raft_entry_t* entry, raft_index_t entry_idx)
{
    WillemtRaftApplication* app = static_cast<WillemtRaftApplication*>(user_data);
    return app->doLogOffer(entry, entry_idx);
}

int WillemtRaftApplication::applylog(raft_server_t* raft, void* user_data,
                                    raft_entry_t* entry, raft_index_t entry_idx)
{
    WillemtRaftApplication* app = static_cast<WillemtRaftApplication*>(user_data);
    return app->doApplyLog(entry, entry_idx);
}

void WillemtRaftApplication::log(raft_server_t* raft, raft_node_t* node,
                                 void* user_data, const char* buf)
{
    // Optional logging
}

int WillemtRaftApplication::persistVote(raft_server_t* raft, void* user_data, raft_node_id_t vote)
{
    return 0;
}

// ============ RAFT LOG HANDLING (CRITICAL FOR QUORUM) ============

int WillemtRaftApplication::doLogOffer(raft_entry_t* entry, raft_index_t entry_idx)
{
    // Called when receiving a log entry to replicate
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

int WillemtRaftApplication::doApplyLog(raft_entry_t* entry, raft_index_t entry_idx)
{
    // CRITICAL: Called ONLY when entry is committed by quorum
    // This guarantees consensus before execution

    if (entry_idx <= lastAppliedIndex_) {
        return 0;  // Already applied
    }

    if (entry->data.len < 1) {
        return 0;
    }

    uint8_t* data = static_cast<uint8_t*>(entry->data.buf);
    LogEntryType type = static_cast<LogEntryType>(data[0]);

    // Handle STATUS_REPORT
    if (type == STATUS_REPORT && entry->data.len >= sizeof(StatusReportEntry) + 1) {
        StatusReportEntry* statusReport = reinterpret_cast<StatusReportEntry*>(data + 1);

        std::cout << std::fixed << std::setprecision(1)
                  << (simTime().dbl() * 1000.0) << "ms Vehicle " << myId_
                  << " APPLYING committed STATUS_REPORT entry #" << entry_idx
                  << " (" << statusReport->numVehicles << " vehicles)" << std::endl;

        // Store committed statuses
        committedStatuses_.clear();
        for (int i = 0; i < statusReport->numVehicles; i++) {
            VehicleStatus& status = statusReport->statuses[i];
            committedStatuses_[status.vehicleId] = status;
        }

        logEntriesCommitted_++;
        lastAppliedIndex_ = entry_idx;

        // Phase 2: Leader proposes pass order
        if (isLeader_ && !hasPassedIntersection_ && !hasCommittedOrder_) {
            timerManager.create(
                veins::TimerSpecification([this]() {
                    if (isLeader_ && !hasPassedIntersection_ && !hasCommittedOrder_) {
                        proposePassOrder();
                    }
                }).oneshotIn(SimTime(0.05))
            );
        }

        return 0;
    }

    // Handle PASS_ORDER (batch-based schedule)
    if (type == PASS_ORDER && entry->data.len >= sizeof(PassScheduleEntry) + 1) {
        PassScheduleEntry* schedule = reinterpret_cast<PassScheduleEntry*>(data + 1);

        std::cout << std::fixed << std::setprecision(1)
                  << (simTime().dbl() * 1000.0) << "ms Vehicle " << myId_
                  << " APPLYING committed PASS_SCHEDULE entry #" << entry_idx 
                  << " with " << schedule->numBatches << " batches" << std::endl;

        // Store committed schedule
        memcpy(&committedSchedule_, schedule, sizeof(PassScheduleEntry));
        hasCommittedOrder_ = true;
        
        // Record when schedule was committed (end of RAFT decision)
        timeOrderCommitted_ = simTime();

        logEntriesCommitted_++;
        lastAppliedIndex_ = entry_idx;

        // Execute batch-based pass order
        applyCommittedPassOrder();

        return 0;
    }

    // Handle VEHICLE_LEFT via RAFT consensus
    if (type == VEHICLE_LEFT && entry->data.len >= sizeof(VehicleLeftEntry) + 1) {
        VehicleLeftEntry* leftEntry = reinterpret_cast<VehicleLeftEntry*>(data + 1);
        
        std::cout << std::fixed << std::setprecision(1)
                  << (simTime().dbl() * 1000.0) << "ms Vehicle " << myId_
                  << " APPLYING VEHICLE_LEFT entry #" << entry_idx
                  << " - vehicle " << leftEntry->vehicleId << " left batch " << leftEntry->batchId << std::endl;
        
        logEntriesCommitted_++;
        lastAppliedIndex_ = entry_idx;
        
        applyVehicleLeftFromRaft(leftEntry->vehicleId, leftEntry->batchId);
        
        // Clean up pending proposals
        auto it = pendingProposals_.find(entry_idx);
        if (it != pendingProposals_.end()) {
            delete[] it->second;
            pendingProposals_.erase(it);
        }
        
        return 0;
    }

    // OLD: Handle PASS_COMMAND (will be removed)
    if (type == PASS_COMMAND && entry->data.len >= sizeof(PassCommandEntry) + 1) {
        PassCommandEntry* cmdData = reinterpret_cast<PassCommandEntry*>(data + 1);
        int vehicleId = cmdData->vehicleId;

        std::cout << std::fixed << std::setprecision(1)
                  << (simTime().dbl() * 1000.0) << "ms Vehicle " << myId_
                  << " APPLYING committed log entry #" << entry_idx
                  << " - PASS command for vehicle " << vehicleId << std::endl;

        logEntriesCommitted_++;
        lastAppliedIndex_ = entry_idx;

        // Execute the pass command (safe because quorum committed it)
        executePassCommand(vehicleId);

        // Clean up pending proposals - old style didn't use uint8_t*
    }

    return 0;
}

// ============ RAFT PROTOCOL MESSAGE SENDING ============

int WillemtRaftApplication::doSendRequestVote(raft_node_t* node, msg_requestvote_t* msg)
{
    if (!node) return -1;

    void* userData = raft_node_get_udata(node);
    int targetVehicleId = static_cast<int>(reinterpret_cast<intptr_t>(userData));

    if (activeVehicles_.find(targetVehicleId) == activeVehicles_.end()) {
        return 0;
    }

    std::vector<uint8_t> data = serializeRequestVote(msg);

    std::ostringstream packetName;
    packetName << "raft-requestvote-from-" << myId_ << "-to-" << targetVehicleId;

    auto payload = makeShared<BytesChunk>(data);
    auto packet = createPacket(packetName.str());
    packet->insertAtBack(payload);
    sendPacket(std::move(packet));

    messagesSent_++;
    return 0;
}

int WillemtRaftApplication::doSendAppendEntries(raft_node_t* node, msg_appendentries_t* msg)
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

    std::ostringstream packetName;
    packetName << "raft-appendentries-from-" << myId_ << "-to-" << targetVehicleId;

    auto payload = makeShared<BytesChunk>(data);
    auto packet = createPacket(packetName.str());
    packet->insertAtBack(payload);
    sendPacket(std::move(packet));

    messagesSent_++;
    return 0;
}


// Continue to Part 3...

// WillemtRaftApplication.cc Part 3 - PACKET PROCESSING AND SERIALIZATION

// ============ PACKET NAME PARSING ============

int WillemtRaftApplication::extractTargetFromPacketName(const std::string& packetName)
{
    size_t toPos = packetName.rfind("-to-");
    if (toPos != std::string::npos) {
        try {
            return std::stoi(packetName.substr(toPos + 4));
        } catch (...) {
            return -1;
        }
    }
    return -1;
}

int WillemtRaftApplication::extractSenderFromPacketName(const std::string& packetName)
{
    size_t fromPos = packetName.find("-from-");
    if (fromPos != std::string::npos) {
        size_t endPos = packetName.find("-", fromPos + 6);
        if (endPos == std::string::npos) {
            endPos = packetName.length();
        }
        try {
            return std::stoi(packetName.substr(fromPos + 6, endPos - fromPos - 6));
        } catch (...) {
            return -1;
        }
    }
    return -1;
}

// ============ PACKET PROCESSING ============

void WillemtRaftApplication::processPacket(std::shared_ptr<Packet> pk)
{
    if (hasPassedIntersection_) return;

    std::string packetName = pk->getName();

    int targetId = extractTargetFromPacketName(packetName);
    bool isBroadcast = (packetName.find("-broadcast") != std::string::npos);

    if (!isBroadcast && targetId != -1 && targetId != myId_) {
        return;
    }

    auto payload = pk->peekAtFront<BytesChunk>();
    if (!payload) return;

    const auto& bytes = payload->getBytes();
    if (bytes.size() > 10000) return;

    messagesReceived_++;

    // Handle RAFT protocol messages
    if (packetName.find("raft-requestvote-response") != std::string::npos) {
        handleRequestVoteResponse(bytes, packetName);
    } else if (packetName.find("raft-requestvote") != std::string::npos) {
        handleRequestVote(bytes, packetName);
    } else if (packetName.find("raft-appendentries-response") != std::string::npos) {
        handleAppendEntriesResponse(bytes, packetName);
    } else if (packetName.find("raft-appendentries") != std::string::npos) {
        handleAppendEntries(bytes, packetName);
    }
    // Handle coordination messages
    else if (packetName.find("coord-status-request") != std::string::npos) {
        int fromLeader = extractSenderFromPacketName(packetName);
        handleStatusRequest(fromLeader);
    } else if (packetName.find("coord-status-response") != std::string::npos) {
        int fromVehicle = extractSenderFromPacketName(packetName);
        bool wos = (bytes.size() > 0 && bytes[0] == 1);
        handleStatusResponse(fromVehicle, wos);
    } else if (packetName.find("coord-vehicle-passed") != std::string::npos) {
        if (bytes.size() >= 1) {
            int vehicleId = static_cast<int>(bytes[0]);
            handleVehiclePassed(vehicleId);
        }
    } else if (packetName.find("coord-vehicle-left") != std::string::npos || 
               packetName.find("coord-vehicle-left-rebroadcast") != std::string::npos) {
        if (bytes.size() >= 1) {
            int vehicleId = static_cast<int>(bytes[0]);
            handleVehicleLeft(vehicleId);
        }
    }
    // Handle discovery and cluster messages (matching WAVE)
    else if (packetName.find("discovery-beacon") != std::string::npos) {
        int senderId = extractSenderFromPacketName(packetName);
        if (senderId != myId_ && bytes.size() >= sizeof(int) + 1) {
            uint8_t senderPhase = bytes[sizeof(int)];
            if (senderPhase == PHASE_DISCOVERY) {
                handleDiscoveryBeacon(senderId, senderPhase);
            } else {
                // Sender is already in a cluster — remove from our discoveredPeers
                discoveredPeers_.erase(senderId);
            }
        }
    }
    else if (packetName.find("cluster-form") != std::string::npos) {
        int senderId = extractSenderFromPacketName(packetName);
        handleClusterForm(bytes, senderId);
    }
    else if (packetName.find("cluster-exists") != std::string::npos) {
        int senderId = extractSenderFromPacketName(packetName);
        handleClusterExists(bytes, senderId);
    }
}

// ============ SERIALIZATION ============

std::vector<uint8_t> WillemtRaftApplication::serializeRequestVote(msg_requestvote_t* msg)
{
    std::vector<uint8_t> data(sizeof(*msg));
    memcpy(data.data(), msg, sizeof(*msg));
    return data;
}

std::vector<uint8_t> WillemtRaftApplication::serializeRequestVoteResponse(msg_requestvote_response_t* msg)
{
    std::vector<uint8_t> data(sizeof(*msg));
    memcpy(data.data(), msg, sizeof(*msg));
    return data;
}

std::vector<uint8_t> WillemtRaftApplication::serializeAppendEntries(msg_appendentries_t* msg)
{
    // Serialize base structure
    size_t baseSize = offsetof(msg_appendentries_t, entries);
    std::vector<uint8_t> data(baseSize);
    memcpy(data.data(), msg, baseSize);
    
    // Serialize entries if present
    if (msg->n_entries > 0 && msg->entries) {
        // Append number of entries
        data.push_back(static_cast<uint8_t>(msg->n_entries));
        
        // Serialize each entry
        for (int i = 0; i < msg->n_entries; i++) {
            raft_entry_t* entry = &msg->entries[i];
            
            // Serialize entry metadata
            data.insert(data.end(), reinterpret_cast<uint8_t*>(&entry->term), 
                       reinterpret_cast<uint8_t*>(&entry->term) + sizeof(entry->term));
            data.insert(data.end(), reinterpret_cast<uint8_t*>(&entry->id), 
                       reinterpret_cast<uint8_t*>(&entry->id) + sizeof(entry->id));
            data.insert(data.end(), reinterpret_cast<uint8_t*>(&entry->type), 
                       reinterpret_cast<uint8_t*>(&entry->type) + sizeof(entry->type));
            
            // Serialize entry data length
            uint32_t dataLen = entry->data.len;
            data.insert(data.end(), reinterpret_cast<uint8_t*>(&dataLen), 
                       reinterpret_cast<uint8_t*>(&dataLen) + sizeof(dataLen));
            
            // Serialize entry data
            if (dataLen > 0 && entry->data.buf) {
                uint8_t* buf = static_cast<uint8_t*>(entry->data.buf);
                data.insert(data.end(), buf, buf + dataLen);
            }
        }
    }
    
    return data;
}

std::vector<uint8_t> WillemtRaftApplication::serializeAppendEntriesResponse(msg_appendentries_response_t* msg)
{
    std::vector<uint8_t> data(sizeof(*msg));
    memcpy(data.data(), msg, sizeof(*msg));
    return data;
}

void WillemtRaftApplication::deserializeRequestVote(const std::vector<uint8_t>& data, msg_requestvote_t* msg)
{
    if (data.size() >= sizeof(*msg)) {
        memcpy(msg, data.data(), sizeof(*msg));
    }
}

void WillemtRaftApplication::deserializeRequestVoteResponse(const std::vector<uint8_t>& data, msg_requestvote_response_t* msg)
{
    if (data.size() >= sizeof(*msg)) {
        memcpy(msg, data.data(), sizeof(*msg));
    }
}

void WillemtRaftApplication::deserializeAppendEntries(const std::vector<uint8_t>& data, msg_appendentries_t* msg)
{
    size_t baseSize = offsetof(msg_appendentries_t, entries);
    if (data.size() < baseSize) {
        msg->entries = nullptr;
        msg->n_entries = 0;
        return;
    }
    
    // Deserialize base structure
    memcpy(msg, data.data(), baseSize);
    
    // Check if there are entries to deserialize
    if (data.size() > baseSize) {
        size_t offset = baseSize;
        
        // Read number of entries
        uint8_t n_entries = data[offset++];
        
        if (n_entries > 0) {
            // Allocate entries array (will be freed by RAFT library or in cleanup)
            msg->entries = new raft_entry_t[n_entries];
            msg->n_entries = n_entries;
            
            // Deserialize each entry
            for (int i = 0; i < n_entries && offset < data.size(); i++) {
                raft_entry_t* entry = &msg->entries[i];
                
                // Deserialize entry metadata
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
                
                // Deserialize entry data length
                uint32_t dataLen = 0;
                if (offset + sizeof(dataLen) <= data.size()) {
                    memcpy(&dataLen, &data[offset], sizeof(dataLen));
                    offset += sizeof(dataLen);
                }
                
                // Deserialize entry data
                if (dataLen > 0 && offset + dataLen <= data.size()) {
                    entry->data.len = dataLen;
                    entry->data.buf = malloc(dataLen);
                    memcpy(entry->data.buf, &data[offset], dataLen);
                    offset += dataLen;
                } else {
                    entry->data.len = 0;
                    entry->data.buf = nullptr;
                }
            }
        } else {
            msg->entries = nullptr;
            msg->n_entries = 0;
        }
    } else {
        msg->entries = nullptr;
        msg->n_entries = 0;
    }
}

void WillemtRaftApplication::deserializeAppendEntriesResponse(const std::vector<uint8_t>& data, msg_appendentries_response_t* msg)
{
    if (data.size() >= sizeof(*msg)) {
        memcpy(msg, data.data(), sizeof(*msg));
    }
}

// ============ RAFT MESSAGE HANDLERS ============

void WillemtRaftApplication::handleRequestVote(const std::vector<uint8_t>& data, const std::string& packetName)
{
    if (!raftServer_) return;

    msg_requestvote_t msg;
    deserializeRequestVote(data, &msg);

    raft_node_t* node = raft_get_node(raftServer_, msg.candidate_id);
    if (!node) return;

    msg_requestvote_response_t response;
    int result = raft_recv_requestvote(raftServer_, node, &msg, &response);

    if (result == 0) {
        std::vector<uint8_t> responseData = serializeRequestVoteResponse(&response);
        int senderVehicleId = msg.candidate_id - 1;

        std::ostringstream responsePacketName;
        responsePacketName << "raft-requestvote-response-from-" << myId_
                          << "-node-" << myRaftNodeId_
                          << "-to-" << senderVehicleId;

        auto payload = makeShared<BytesChunk>(responseData);
        auto packet = createPacket(responsePacketName.str());
        packet->insertAtBack(payload);
        sendPacket(std::move(packet));
        messagesSent_++;
    }
}

void WillemtRaftApplication::handleRequestVoteResponse(const std::vector<uint8_t>& data, const std::string& packetName)
{
    if (!raftServer_) return;

    msg_requestvote_response_t msg;
    deserializeRequestVoteResponse(data, &msg);

    size_t nodePos = packetName.find("-node-");
    size_t toPos = packetName.find("-to-", nodePos);

    if (nodePos != std::string::npos && toPos != std::string::npos) {
        try {
            raft_node_id_t senderNodeId = std::stoi(packetName.substr(nodePos + 6, toPos - nodePos - 6));
            raft_node_t* node = raft_get_node(raftServer_, senderNodeId);
            if (node) {
                raft_recv_requestvote_response(raftServer_, node, &msg);
            }
        } catch (...) {}
    }
}

void WillemtRaftApplication::handleAppendEntries(const std::vector<uint8_t>& data, const std::string& packetName)
{
    if (!raftServer_) return;

    msg_appendentries_t msg;
    deserializeAppendEntries(data, &msg);

    int senderVehicleId = extractSenderFromPacketName(packetName);
    if (senderVehicleId < 0) return;

    std::cout << std::fixed << std::setprecision(1)
              << (simTime().dbl() * 1000.0) << "ms Vehicle " << myId_
              << " RECEIVED AppendEntries from vehicle " << senderVehicleId
              << " (n_entries=" << msg.n_entries << ", leader_commit=" << msg.leader_commit << ")" << std::endl;

    raft_node_id_t senderNodeId = senderVehicleId + 1;
    raft_node_t* node = raft_get_node(raftServer_, senderNodeId);

    if (node) {
        msg_appendentries_response_t response;
        raft_recv_appendentries(raftServer_, node, &msg, &response);

        std::vector<uint8_t> responseData = serializeAppendEntriesResponse(&response);

        std::ostringstream responsePacketName;
        responsePacketName << "raft-appendentries-response-from-" << myId_
                          << "-node-" << myRaftNodeId_
                          << "-to-" << senderVehicleId;

        auto payload = makeShared<BytesChunk>(responseData);
        auto packet = createPacket(responsePacketName.str());
        packet->insertAtBack(payload);
        sendPacket(std::move(packet));
        messagesSent_++;
    }
}

void WillemtRaftApplication::handleAppendEntriesResponse(const std::vector<uint8_t>& data, const std::string& packetName)
{
    if (!raftServer_) return;

    msg_appendentries_response_t msg;
    deserializeAppendEntriesResponse(data, &msg);

    size_t nodePos = packetName.find("-node-");
    size_t toPos = packetName.find("-to-", nodePos);

    if (nodePos != std::string::npos && toPos != std::string::npos) {
        try {
            raft_node_id_t senderNodeId = std::stoi(packetName.substr(nodePos + 6, toPos - nodePos - 6));
            raft_node_t* node = raft_get_node(raftServer_, senderNodeId);
            if (node) {
                raft_recv_appendentries_response(raftServer_, node, &msg);
            }
        } catch (...) {}
    }
}

// Continue to Part 4...

// WillemtRaftApplication.cc Part 4 - COORDINATION PROTOCOL WITH QUORUM CONSENSUS

// ============ STATUS COLLECTION ============

void WillemtRaftApplication::sendStatusRequest()
{
    std::ostringstream packetName;
    packetName << "coord-status-request-from-" << myId_ << "-broadcast";

    std::vector<uint8_t> data = {static_cast<uint8_t>(myId_)};
    auto payload = makeShared<BytesChunk>(data);
    auto packet = createPacket(packetName.str());
    packet->insertAtBack(payload);
    sendPacket(std::move(packet));

    messagesSent_++;
    waitingForStatus_ = true;
    statusResponseCount_ = 0;
    collectedWayOfSight_.clear();

    // Include self
    collectedWayOfSight_[myId_] = wayOfSight_;
    statusResponseCount_ = 1;

    // Timeout for status collection
    timerManager.create(
        veins::TimerSpecification([this]() {
            if (waitingForStatus_ && isLeader_ && !hasPassedIntersection_) {
                collectStatusAndDecide();
            }
        }).oneshotIn(SimTime(statusCollectionTimeoutMs_ / 1000.0))
    );
}

void WillemtRaftApplication::handleStatusRequest(int fromLeader)
{
    if (hasPassedIntersection_) return;
    sendStatusResponse(fromLeader, wayOfSight_);
}

void WillemtRaftApplication::sendStatusResponse(int toLeader, bool wos)
{
    std::ostringstream packetName;
    packetName << "coord-status-response-from-" << myId_ << "-to-" << toLeader;

    std::vector<uint8_t> data = {static_cast<uint8_t>(wos ? 1 : 0)};
    auto payload = makeShared<BytesChunk>(data);
    auto packet = createPacket(packetName.str());
    packet->insertAtBack(payload);
    sendPacket(std::move(packet));

    messagesSent_++;
}

void WillemtRaftApplication::handleStatusResponse(int fromVehicle, bool wos)
{
    if (!isLeader_ || !waitingForStatus_) return;

    collectedWayOfSight_[fromVehicle] = wos;
    statusResponseCount_++;

    int expectedResponses = activeVehicles_.size();
    if (statusResponseCount_ >= expectedResponses) {
        collectStatusAndDecide();
    }
}

// ============ TWO-PHASE CONSENSUS ============

void WillemtRaftApplication::collectStatusAndDecide()
{
    if (!isLeader_ || hasPassedIntersection_ || hasCommittedOrder_) return;

    waitingForStatus_ = false;

    std::cout << std::fixed << std::setprecision(1)
              << (simTime().dbl() * 1000.0) << "ms Leader " << myId_
              << " collected status from " << collectedWayOfSight_.size() << " vehicles" << std::endl;

    // Phase 1: Propose status report to quorum
    proposeStatusReport();
}

void WillemtRaftApplication::proposeStatusReport()
{
    if (!raftServer_ || !isLeader_) return;

    std::cout << std::fixed << std::setprecision(1)
              << (simTime().dbl() * 1000.0) << "ms Leader " << myId_
              << " PROPOSING STATUS_REPORT to quorum" << std::endl;

    // Create status report entry
    StatusReportEntry statusReport;
    statusReport.numVehicles = 0;

    // CRITICAL: Include ALL active vehicles, not just the ones that responded
    // Use activeVehicles_ set to ensure we include everyone
    for (int vid : activeVehicles_) {
        if (statusReport.numVehicles >= 32) break;
        
        VehicleStatus& status = statusReport.statuses[statusReport.numVehicles];
        status.vehicleId = vid;
        
        // Get wayOfSight from collected responses, default to false if not received
        status.wayOfSight = collectedWayOfSight_.count(vid) > 0 ? collectedWayOfSight_[vid] : false;
        
        // Get lane info
        int vehiclesPerSide = totalVehicles_ / 4;
        if (vehiclesPerSide == 0) vehiclesPerSide = 1;
        int sideIndex = vid / vehiclesPerSide;
        switch (sideIndex % 4) {
            case 0: strncpy(status.lane, "N2C", 7); break;
            case 1: strncpy(status.lane, "S2C", 7); break;
            case 2: strncpy(status.lane, "E2C", 7); break;
            case 3: strncpy(status.lane, "W2C", 7); break;
        }
        status.lane[7] = '\0';
        status.positionInLane = vid % vehiclesPerSide;
        
        statusReport.numVehicles++;
    }

    std::cout << std::fixed << std::setprecision(1)
              << (simTime().dbl() * 1000.0) << "ms Leader " << myId_
              << " including " << statusReport.numVehicles << " vehicles in STATUS_REPORT" << std::endl;

    // Prepare entry buffer: [type byte][status report data]
    size_t entrySize = 1 + sizeof(StatusReportEntry);
    std::vector<uint8_t> entryBuffer(entrySize);
    entryBuffer[0] = static_cast<uint8_t>(STATUS_REPORT);
    memcpy(entryBuffer.data() + 1, &statusReport, sizeof(StatusReportEntry));

    // Create Raft entry
    raft_entry_t entry;
    memset(&entry, 0, sizeof(entry));
    entry.term = raft_get_current_term(raftServer_);
    entry.id = 0;
    entry.type = RAFT_LOGTYPE_NORMAL;
    entry.data.buf = entryBuffer.data();
    entry.data.len = entrySize;

    // Submit to Raft for replication
    msg_entry_response_t response;
    int result = raft_recv_entry(raftServer_, &entry, &response);

    if (result == 0) {
        logEntriesProposed_++;
        std::cout << std::fixed << std::setprecision(1)
                  << (simTime().dbl() * 1000.0) << "ms Leader " << myId_
                  << " submitted STATUS_REPORT entry #" << response.idx << std::endl;
        
        // Leader must explicitly apply its own entry and trigger proposePassOrder
        timerManager.create(
            veins::TimerSpecification([this, idx = response.idx, statusReport]() {
                if (raftServer_ && raft_get_commit_idx(raftServer_) >= idx && !hasCommittedOrder_) {
                    std::cout << std::fixed << std::setprecision(1)
                              << (simTime().dbl() * 1000.0) << "ms Leader " << myId_
                              << " applying committed STATUS_REPORT #" << idx << std::endl;
                    
                    // Store committed statuses
                    committedStatuses_.clear();
                    for (int i = 0; i < statusReport.numVehicles; i++) {
                        committedStatuses_[statusReport.statuses[i].vehicleId] = statusReport.statuses[i];
                    }
                    
                    logEntriesCommitted_++;
                    lastAppliedIndex_ = idx;
                    
                    // Phase 2: Propose pass order
                    if (isLeader_ && !hasPassedIntersection_ && !hasCommittedOrder_) {
                        proposePassOrder();
                    }
                }
            }).oneshotIn(SimTime(0.1))
        );
    } else {
        std::cout << std::fixed << std::setprecision(1)
                  << (simTime().dbl() * 1000.0) << "ms Leader " << myId_
                  << " FAILED to propose STATUS_REPORT (error " << result << ")" << std::endl;
    }
}

int WillemtRaftApplication::getLaneIndex(const std::string& lane)
{
    if (lane == "N2C") return 0;
    if (lane == "S2C") return 1;
    if (lane == "E2C") return 2;
    if (lane == "W2C") return 3;
    return 0;
}

std::vector<int> WillemtRaftApplication::createDeterministicOrder()
{
    std::vector<int> order;
    std::vector<int> frontVehicles;
    std::vector<int> blockedVehicles;

    // 1. Separate vehicles by wayOfSight
    for (const auto& kv : committedStatuses_) {
        int vid = kv.first;
        const VehicleStatus& status = kv.second;
        if (status.wayOfSight) {
            frontVehicles.push_back(vid);
        } else {
            blockedVehicles.push_back(vid);
        }
    }

    // 2. Sort front vehicles by lane: N, S, E, W
    std::sort(frontVehicles.begin(), frontVehicles.end(), [this](int a, int b) {
        return getLaneIndex(committedStatuses_[a].lane) < getLaneIndex(committedStatuses_[b].lane);
    });

    // 3. Add front vehicles to order
    for (int vid : frontVehicles) {
        order.push_back(vid);
    }

    // 4. Add blocked vehicles after their lane-mates
    for (int vid : blockedVehicles) {
        std::string myLane = committedStatuses_[vid].lane;
        
        // Find where to insert (after last vehicle from same lane)
        int insertPos = order.size();
        for (int i = order.size() - 1; i >= 0; i--) {
            if (committedStatuses_[order[i]].lane == myLane) {
                insertPos = i + 1;
                break;
            }
        }
        
        order.insert(order.begin() + insertPos, vid);
    }

    return order;
}

void WillemtRaftApplication::proposePassOrder()
{
    if (!raftServer_ || !isLeader_) return;

    // Build schedule from committed statuses (converted to proposals)
    std::vector<VehicleProposal> pool;
    for (auto& kv : committedStatuses_) {
        VehicleProposal prop;
        memset(&prop, 0, sizeof(prop));
        prop.vehicleId = kv.second.vehicleId;
        strncpy(prop.laneEdgeId, kv.second.lane, sizeof(prop.laneEdgeId) - 1);
        prop.laneIndex = getLaneIndex(kv.second.lane);
        prop.intendedTurn = 0;  // STRAIGHT for this benchmark
        prop.isFirstInLane = kv.second.wayOfSight;
        prop.blockedByVehicleId = kv.second.wayOfSight ? -1 : (kv.second.vehicleId - 1);
        pool.push_back(prop);
    }
    
    // Sort by priority: isFirstInLane DESC, laneIndex ASC, vehicleId ASC
    std::sort(pool.begin(), pool.end(), [](const VehicleProposal& a, const VehicleProposal& b) {
        if (a.isFirstInLane != b.isFirstInLane) return a.isFirstInLane > b.isFirstInLane;
        if (a.laneIndex != b.laneIndex) return a.laneIndex < b.laneIndex;
        return a.vehicleId < b.vehicleId;
    });
    
    // Create batch-based schedule
    PassScheduleEntry schedule;
    memset(&schedule, 0, sizeof(schedule));
    
    while (!pool.empty() && schedule.numBatches < 16) {
        PassBatch& currentBatch = schedule.batches[schedule.numBatches];
        currentBatch.numVehicles = 0;
        
        std::vector<VehicleProposal> remaining;
        for (auto& prop : pool) {
            bool canAdd = true;
            
            // Check conflict with vehicles already in this batch
            for (int i = 0; i < currentBatch.numVehicles; i++) {
                // Find the proposal for the vehicle in batch
                int batchVid = currentBatch.vehicleIds[i];
                int batchLane = -1;
                int batchTurn = 0;
                for (auto& p : committedStatuses_) {
                    if (p.second.vehicleId == batchVid) {
                        batchLane = getLaneIndex(p.second.lane);
                        break;
                    }
                }
                
                if (movementsConflict(prop.laneIndex, prop.intendedTurn, batchLane, batchTurn)) {
                    canAdd = false;
                    break;
                }
            }
            
            // Check if blocked by vehicle not yet passed
            if (canAdd && prop.blockedByVehicleId >= 0) {
                bool blockerInPreviousBatch = false;
                for (int b = 0; b < schedule.numBatches; b++) {
                    for (int v = 0; v < schedule.batches[b].numVehicles; v++) {
                        if (schedule.batches[b].vehicleIds[v] == prop.blockedByVehicleId) {
                            blockerInPreviousBatch = true;
                            break;
                        }
                    }
                    if (blockerInPreviousBatch) break;
                }
                // Also check current batch
                for (int v = 0; v < currentBatch.numVehicles; v++) {
                    if (currentBatch.vehicleIds[v] == prop.blockedByVehicleId) {
                        blockerInPreviousBatch = true;
                        break;
                    }
                }
                if (!blockerInPreviousBatch) {
                    canAdd = false;  // Blocker hasn't been scheduled yet
                }
            }
            
            if (canAdd && currentBatch.numVehicles < 8) {
                currentBatch.vehicleIds[currentBatch.numVehicles++] = prop.vehicleId;
            } else {
                remaining.push_back(prop);
            }
        }
        
        if (currentBatch.numVehicles > 0) {
            schedule.numBatches++;
        }
        pool = remaining;
        
        // Safety: prevent infinite loop
        if (remaining.size() == pool.size() && !pool.empty()) {
            // Force add one vehicle to break deadlock
            if (schedule.numBatches < 16) {
                PassBatch& forceBatch = schedule.batches[schedule.numBatches];
                forceBatch.numVehicles = 1;
                forceBatch.vehicleIds[0] = pool[0].vehicleId;
                schedule.numBatches++;
                pool.erase(pool.begin());
            }
        }
    }
    
    // Log the schedule
    std::cout << std::fixed << std::setprecision(1)
              << (simTime().dbl() * 1000.0) << "ms Leader " << myId_
              << " PROPOSING PASS_SCHEDULE with " << schedule.numBatches << " batches:" << std::endl;
    for (int b = 0; b < schedule.numBatches; b++) {
        std::cout << "  Batch " << b << ": [";
        for (int v = 0; v < schedule.batches[b].numVehicles; v++) {
            std::cout << schedule.batches[b].vehicleIds[v];
            if (v < schedule.batches[b].numVehicles - 1) std::cout << ",";
        }
        std::cout << "]" << std::endl;
    }

    // Prepare entry buffer: [type byte][schedule data]
    size_t entrySize = 1 + sizeof(PassScheduleEntry);
    uint8_t* data = new uint8_t[entrySize];
    data[0] = static_cast<uint8_t>(PASS_ORDER);
    memcpy(data + 1, &schedule, sizeof(PassScheduleEntry));

    // Create Raft entry
    raft_entry_t entry;
    memset(&entry, 0, sizeof(entry));
    entry.term = raft_get_current_term(raftServer_);
    entry.id = 0;
    entry.type = RAFT_LOGTYPE_NORMAL;
    entry.data.buf = data;
    entry.data.len = entrySize;

    // Submit to Raft for replication
    msg_entry_response_t response;
    int result = raft_recv_entry(raftServer_, &entry, &response);

    if (result == 0) {
        logEntriesProposed_++;
        pendingProposals_[response.idx] = data;
        
        std::cout << std::fixed << std::setprecision(1)
                  << (simTime().dbl() * 1000.0) << "ms Leader " << myId_
                  << " submitted PASS_SCHEDULE entry #" << response.idx << std::endl;
        
        // Apply schedule after commit
        timerManager.create(
            veins::TimerSpecification([this, idx = response.idx, schedule]() {
                if (raftServer_ && raft_get_commit_idx(raftServer_) >= idx && !hasCommittedOrder_) {
                    std::cout << std::fixed << std::setprecision(1)
                              << (simTime().dbl() * 1000.0) << "ms Leader " << myId_
                              << " APPLYING committed PASS_SCHEDULE #" << idx << std::endl;
                    
                    memcpy(&committedSchedule_, &schedule, sizeof(PassScheduleEntry));
                    hasCommittedOrder_ = true;
                    timeOrderCommitted_ = simTime();
                    logEntriesCommitted_++;
                    lastAppliedIndex_ = idx;
                    
                    applyCommittedPassOrder();
                }
            }).oneshotIn(SimTime(0.1))
        );
    } else {
        std::cout << std::fixed << std::setprecision(1)
                  << (simTime().dbl() * 1000.0) << "ms Leader " << myId_
                  << " FAILED to propose PASS_SCHEDULE (error " << result << ")" << std::endl;
        delete[] data;
    }
}

// ============ EXECUTION AND BROADCAST HANDLERS ============

int WillemtRaftApplication::findMyPositionInOrder()
{
    for (int i = 0; i < committedPassOrder_.numVehicles; i++) {
        if (committedPassOrder_.order[i] == myId_) {
            return i;
        }
    }
    return -1;
}

void WillemtRaftApplication::executePassOrder()
{
    // Legacy function - redirect to batch-based execution
    applyCommittedPassOrder();
}

void WillemtRaftApplication::applyCommittedPassOrder()
{
    if (hasPassedIntersection_) return;
    
    // Find which batch I'm in
    myBatch_ = -1;
    for (int b = 0; b < committedSchedule_.numBatches; b++) {
        for (int v = 0; v < committedSchedule_.batches[b].numVehicles; v++) {
            if (committedSchedule_.batches[b].vehicleIds[v] == myId_) {
                myBatch_ = b;
                break;
            }
        }
        if (myBatch_ >= 0) break;
    }
    
    if (myBatch_ < 0) {
        std::cout << std::fixed << std::setprecision(1)
                  << (simTime().dbl() * 1000.0) << "ms Vehicle " << myId_
                  << " NOT SCHEDULED - using fallback in " << (committedSchedule_.numBatches * 2000.0) << "ms" << std::endl;
        
        // Fallback: schedule to move after all batches complete
        int fallbackDelayMs = committedSchedule_.numBatches * 2000;
        timerManager.create(
            veins::TimerSpecification([this]() {
                if (!hasPassedIntersection_) {
                    std::cout << std::fixed << std::setprecision(1)
                              << (simTime().dbl() * 1000.0) << "ms Vehicle " << myId_
                              << " FALLBACK movement starting" << std::endl;
                    isFallbackMode_ = true;
                    coordinationMethod_ = "fallback";
                    resumeMovement();
                }
            }).oneshotIn(SimTime(fallbackDelayMs / 1000.0))
        );
        return;
    }
    
    std::cout << std::fixed << std::setprecision(1)
              << (simTime().dbl() * 1000.0) << "ms Vehicle " << myId_
              << " assigned to batch " << myBatch_ << " (current batch: " << currentBatch_ << ")" << std::endl;
    
    if (myBatch_ == 0) {
        // I'm in first batch, start immediately
        std::cout << std::fixed << std::setprecision(1)
                  << (simTime().dbl() * 1000.0) << "ms Vehicle " << myId_
                  << " in BATCH 0, starting immediately" << std::endl;
        resumeMovement();
    } else {
        // Schedule safety timer to force movement if batch doesn't advance
        int safetyDelayMs = myBatch_ * 2000;  // 2 seconds per batch
        timerManager.create(
            veins::TimerSpecification([this, expectedBatch = myBatch_]() {
                if (!hasPassedIntersection_ && currentBatch_ < expectedBatch) {
                    std::cout << std::fixed << std::setprecision(1)
                              << (simTime().dbl() * 1000.0) << "ms Vehicle " << myId_
                              << " BATCH SAFETY TIMEOUT - forcing movement for batch " << expectedBatch << std::endl;
                    currentBatch_ = expectedBatch;
                    resumeMovement();
                }
            }).oneshotIn(SimTime(safetyDelayMs / 1000.0))
        );
        
        std::cout << std::fixed << std::setprecision(1)
                  << (simTime().dbl() * 1000.0) << "ms Vehicle " << myId_
                  << " waiting for batch " << myBatch_ << " (safety timer: " << safetyDelayMs << "ms)" << std::endl;
    }
}

bool WillemtRaftApplication::movementsConflict(int laneA, int turnA, int laneB, int turnB)
{
    // Conflict matrix for intersection movements
    // Lanes: 0=W, 1=S, 2=E, 3=N
    // Turns: 0=STRAIGHT, 1=LEFT, 2=RIGHT
    
    if (laneA == laneB) return true;  // Same direction always conflicts
    
    // Opposing straight movements don't conflict: W(0)↔E(2), N(3)↔S(1)
    bool opposing = (laneA + laneB == 2) || (laneA + laneB == 4);
    
    if (opposing && turnA == 0 && turnB == 0) return false;  // Opposing straight = OK
    if (opposing && turnA == 1 && turnB == 1) return false;  // Opposing left = OK
    
    // All other combinations conflict (conservative)
    return true;
}

void WillemtRaftApplication::applyVehicleLeftFromRaft(int vehicleId, int batchId)
{
    std::cout << std::fixed << std::setprecision(1)
              << (simTime().dbl() * 1000.0) << "ms Vehicle " << myId_
              << " RAFT-confirmed: vehicle " << vehicleId << " left batch " << batchId << std::endl;
    
    // Remove from active vehicles
    activeVehicles_.erase(vehicleId);
    vehiclesLeftInBatch_.insert(vehicleId);
    
    // Check if batch is complete
    checkBatchAdvance();
}

void WillemtRaftApplication::checkBatchAdvance()
{
    if (hasPassedIntersection_ || !hasCommittedOrder_) return;
    if (currentBatch_ >= committedSchedule_.numBatches) return;
    
    // Check if all vehicles in current batch have left
    PassBatch& batch = committedSchedule_.batches[currentBatch_];
    bool allLeft = true;
    for (int v = 0; v < batch.numVehicles; v++) {
        if (vehiclesLeftInBatch_.find(batch.vehicleIds[v]) == vehiclesLeftInBatch_.end()) {
            allLeft = false;
            break;
        }
    }
    
    if (allLeft) {
        currentBatch_++;
        vehiclesLeftInBatch_.clear();
        
        std::cout << std::fixed << std::setprecision(1)
                  << (simTime().dbl() * 1000.0) << "ms Vehicle " << myId_
                  << " BATCH ADVANCE: now on batch " << currentBatch_ << std::endl;
        
        // If it's my turn, start moving
        if (myBatch_ == currentBatch_ && !hasPassedIntersection_) {
            std::cout << std::fixed << std::setprecision(1)
                      << (simTime().dbl() * 1000.0) << "ms Vehicle " << myId_
                      << " MY BATCH - starting movement" << std::endl;
            resumeMovement();
        }
    }
}

void WillemtRaftApplication::proposeVehicleLeft(int vehicleId, int batchId)
{
    if (!raftServer_ || !isLeader_ || hasPassedIntersection_) return;
    
    VehicleLeftEntry entryData;
    entryData.vehicleId = vehicleId;
    entryData.batchId = batchId;
    
    size_t entrySize = 1 + sizeof(VehicleLeftEntry);
    uint8_t* data = new uint8_t[entrySize];
    data[0] = VEHICLE_LEFT;
    memcpy(data + 1, &entryData, sizeof(VehicleLeftEntry));
    
    raft_entry_t entry;
    memset(&entry, 0, sizeof(entry));
    entry.term = raft_get_current_term(raftServer_);
    entry.id = 0;
    entry.type = RAFT_LOGTYPE_NORMAL;
    entry.data.buf = data;
    entry.data.len = entrySize;
    
    msg_entry_response_t response;
    int result = raft_recv_entry(raftServer_, &entry, &response);
    
    if (result == 0) {
        logEntriesProposed_++;
        pendingProposals_[response.idx] = data;
        std::cout << std::fixed << std::setprecision(1)
                  << (simTime().dbl() * 1000.0) << "ms Leader " << myId_
                  << " proposed VEHICLE_LEFT for " << vehicleId << " batch " << batchId << std::endl;
    } else {
        EV_WARN << "Vehicle " << myId_ << " failed to propose VEHICLE_LEFT: " << result << endl;
        delete[] data;
    }
}

WillemtRaftApplication::VehicleProposal WillemtRaftApplication::buildMyProposal()
{
    VehicleProposal proposal;
    memset(&proposal, 0, sizeof(proposal));
    proposal.vehicleId = myId_;
    strncpy(proposal.laneEdgeId, myLane_.c_str(), sizeof(proposal.laneEdgeId) - 1);
    proposal.laneIndex = myLaneIndex_;
    proposal.intendedTurn = 0;  // STRAIGHT for this benchmark
    proposal.blockedByVehicleId = detectBlockingVehicle();
    proposal.isFirstInLane = (proposal.blockedByVehicleId == -1);
    
    if (timeStopped_ > SIMTIME_ZERO) {
        proposal.waitingTimeMs = (simTime() - timeStopped_).dbl() * 1000.0;
    } else {
        proposal.waitingTimeMs = 0.0;
    }
    
    proposal.distanceToJunction = calculateDistanceToJunction();
    
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

int WillemtRaftApplication::detectBlockingVehicle()
{
    // Simple detection: use vehicle in front from lane calculation
    return vehicleInFrontOfMe_;
}

double WillemtRaftApplication::calculateDistanceToJunction()
{
    // Use the same calculation as getDistanceToJunction()
    return getDistanceToJunction();
}

void WillemtRaftApplication::checkIfPreviousVehicleLeft(int previousVehicleId)
{
    if (hasPassedIntersection_) return;
    
    // Use TraCI to check if previous vehicle is still at intersection
    // This simulates real-world sensors detecting vehicle presence
    bool previousStillAtIntersection = false;
    
    try {
        if (traci_) {
            // Construct SUMO vehicle ID
            std::string previousVehicleSumoId = "flow.0." + std::to_string(previousVehicleId);
            
            try {
                // Query SUMO for vehicle's current road
                std::string roadId = traci_->vehicle(previousVehicleSumoId).getRoadId();
                
                // Intersection edges start with ':'
                previousStillAtIntersection = (!roadId.empty() && roadId[0] == ':');
            } catch (...) {
                // Vehicle doesn't exist or error - assume it left
                previousStillAtIntersection = false;
            }
        }
    } catch (...) {
        previousStillAtIntersection = false;
    }
    
    if (!previousStillAtIntersection) {
        std::cout << std::fixed << std::setprecision(1)
                  << (simTime().dbl() * 1000.0) << "ms Vehicle " << myId_
                  << " detected vehicle " << previousVehicleId << " left (via TraCI), waiting 50ms safety gap" << std::endl;
        
        // Add 50ms safety gap to prevent all vehicles moving simultaneously
        // when they all poll at the same simulation tick
        timerManager.create(
            veins::TimerSpecification([this]() {
                if (!hasPassedIntersection_) {
                    std::cout << std::fixed << std::setprecision(1)
                              << (simTime().dbl() * 1000.0) << "ms Vehicle " << myId_
                              << " moving now (after safety gap)" << std::endl;
                    resumeMovement();
                }
            }).oneshotIn(SimTime(0.05))  // 50ms safety gap
        );
    }
}

void WillemtRaftApplication::sendVehicleLeft()
{
    std::cout << std::fixed << std::setprecision(1)
              << (simTime().dbl() * 1000.0) << "ms Vehicle " << myId_
              << " sending vehicle-left (isLeader=" << isLeader_ << ", myBatch=" << myBatch_ << ")" << std::endl;

    if (isLeader_) {
        // Leader: propose via RAFT consensus
        proposeVehicleLeft(myId_, myBatch_);
    } else {
        // Follower: broadcast to leader (who will propose to RAFT)
        std::ostringstream packetName;
        packetName << "coord-vehicle-left-from-" << myId_ << "-batch-" << myBatch_;

        std::vector<uint8_t> data(sizeof(int) * 2);
        memcpy(data.data(), &myId_, sizeof(int));
        memcpy(data.data() + sizeof(int), &myBatch_, sizeof(int));
        
        auto payload = makeShared<BytesChunk>(data);
        auto packet = createPacket(packetName.str());
        packet->insertAtBack(payload);
        sendPacket(std::move(packet));

        messagesSent_++;
    }
}

void WillemtRaftApplication::handleVehicleLeft(int vehicleId)
{
    // This is called when we receive a VEHICLE_LEFT broadcast (not via RAFT)
    // If we're the leader, propose it to RAFT for consensus
    // If not, just update locally (but RAFT will provide authoritative update)
    
    std::cout << std::fixed << std::setprecision(1)
              << (simTime().dbl() * 1000.0) << "ms Vehicle " << myId_
              << " RECEIVED vehicle-left from vehicle " << vehicleId << std::endl;

    if (isLeader_) {
        // Find the batch for this vehicle
        int batchId = -1;
        for (int b = 0; b < committedSchedule_.numBatches; b++) {
            for (int v = 0; v < committedSchedule_.batches[b].numVehicles; v++) {
                if (committedSchedule_.batches[b].vehicleIds[v] == vehicleId) {
                    batchId = b;
                    break;
                }
            }
            if (batchId >= 0) break;
        }
        
        if (batchId >= 0) {
            proposeVehicleLeft(vehicleId, batchId);
        }
    }
    
    // Local optimistic update (RAFT will confirm)
    activeVehicles_.erase(vehicleId);
    vehiclesLeftInBatch_.insert(vehicleId);
    checkBatchAdvance();
}

// ============ OLD FUNCTIONS (WILL BE REMOVED) ============


int WillemtRaftApplication::selectVehicleToPass()
{
    std::vector<int> canPassVehicles;
    std::vector<int> canPassNonLeaders;
    int vehiclesPerSide = totalVehicles_ / 4;
    if (vehiclesPerSide == 0) vehiclesPerSide = 1;

    // Collect from reported statuses
    for (const auto& entry : collectedWayOfSight_) {
        if (entry.second && activeVehicles_.count(entry.first) > 0) {
            canPassVehicles.push_back(entry.first);
            if (entry.first != myId_) {  // Track non-leaders separately
                canPassNonLeaders.push_back(entry.first);
            }
        }
    }

    // Calculate based on active vehicles
    for (int vid : activeVehicles_) {
        int sideIndex = vid / vehiclesPerSide;
        int positionInLane = vid % vehiclesPerSide;
        int laneStartId = sideIndex * vehiclesPerSide;

        bool hasWayOfSight = true;

        for (int pos = 0; pos < positionInLane; pos++) {
            int vehicleInFront = laneStartId + pos;
            if (activeVehicles_.count(vehicleInFront) > 0) {
                hasWayOfSight = false;
                break;
            }
        }

        if (hasWayOfSight) {
            canPassVehicles.push_back(vid);
            if (vid != myId_) {  // Track non-leaders separately
                canPassNonLeaders.push_back(vid);
            }
        }
    }

    // Remove duplicates
    std::sort(canPassVehicles.begin(), canPassVehicles.end());
    canPassVehicles.erase(std::unique(canPassVehicles.begin(), canPassVehicles.end()),
                          canPassVehicles.end());
    
    std::sort(canPassNonLeaders.begin(), canPassNonLeaders.end());
    canPassNonLeaders.erase(std::unique(canPassNonLeaders.begin(), canPassNonLeaders.end()),
                           canPassNonLeaders.end());

    // CRITICAL: Prioritize non-leader vehicles to keep coordination going
    // Only select the leader if no other vehicles can pass
    std::vector<int>& selectionPool = canPassNonLeaders.empty() ? canPassVehicles : canPassNonLeaders;
    
    if (selectionPool.empty()) {
        return -1;
    }

    // Random selection from the prioritized pool
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, selectionPool.size() - 1);

    int selected = selectionPool[dis(gen)];
    
    std::cout << std::fixed << std::setprecision(1)
              << (simTime().dbl() * 1000.0) << "ms Leader " << myId_
              << " selected vehicle " << selected << " to pass"
              << " (pool: " << selectionPool.size() << " candidates)" << std::endl;

    return selected;
}

// ============ QUORUM CONSENSUS FUNCTIONS (CRITICAL) ============

void WillemtRaftApplication::proposePassCommand(int vehicleId)
{
    if (!raftServer_ || !isLeader_) return;

    std::cout << std::fixed << std::setprecision(1)
              << (simTime().dbl() * 1000.0) << "ms Leader " << myId_
              << " PROPOSING to Raft log: Vehicle " << vehicleId << " should PASS" << std::endl;

    // Create log entry data
    PassCommandEntry cmdData;
    cmdData.vehicleId = vehicleId;
    cmdData.proposedTime = simTime();

    // Prepare entry buffer: [type byte][command data]
    size_t entrySize = 1 + sizeof(PassCommandEntry);
    std::vector<uint8_t> entryBuffer(entrySize);
    entryBuffer[0] = static_cast<uint8_t>(PASS_COMMAND);
    memcpy(entryBuffer.data() + 1, &cmdData, sizeof(PassCommandEntry));

    // Create Raft entry
    raft_entry_t entry;
    memset(&entry, 0, sizeof(entry));
    entry.term = raft_get_current_term(raftServer_);
    entry.id = 0;
    entry.type = RAFT_LOGTYPE_NORMAL;
    entry.data.buf = entryBuffer.data();
    entry.data.len = entrySize;

    // Submit to Raft for replication
    msg_entry_response_t response;
    int result = raft_recv_entry(raftServer_, &entry, &response);

    if (result == 0) {
        logEntriesProposed_++;
        // Note: legacy function - not storing in pendingProposals_

        std::cout << std::fixed << std::setprecision(1)
                  << (simTime().dbl() * 1000.0) << "ms Leader " << myId_
                  << " submitted log entry #" << response.idx
                  << " (waiting for quorum commit...)" << std::endl;
        
        // CRITICAL: Schedule a check to see if entry is committed
        // The leader needs to execute the command too, but RAFT library
        // only calls applylog on followers, not the leader
        timerManager.create(
            veins::TimerSpecification([this, vehicleId, idx = response.idx]() {
                if (raftServer_ && raft_get_commit_idx(raftServer_) >= idx) {
                    // Entry is committed, execute it
                    std::cout << std::fixed << std::setprecision(1)
                              << (simTime().dbl() * 1000.0) << "ms Leader " << myId_
                              << " APPLYING committed log entry #" << idx 
                              << " - PASS command for vehicle " << vehicleId << std::endl;
                    
                    logEntriesCommitted_++;
                    executePassCommand(vehicleId);
                }
            }).oneshotIn(SimTime(0.1))
        );
    } else {
        std::cout << std::fixed << std::setprecision(1)
                  << (simTime().dbl() * 1000.0) << "ms Leader " << myId_
                  << " FAILED to propose entry (error " << result << ")" << std::endl;

        // Retry
        timerManager.create(
            veins::TimerSpecification([this]() {
                if (isLeader_ && !hasPassedIntersection_) {
                    sendStatusRequest();
                }
            }).oneshotIn(SimTime(0.1))
        );
    }
}

void WillemtRaftApplication::executePassCommand(int vehicleId)
{
    // Called from doApplyLog() ONLY after quorum commits the entry
    if (hasPassedIntersection_) return;

    std::cout << std::fixed << std::setprecision(1)
              << (simTime().dbl() * 1000.0) << "ms Vehicle " << myId_
              << " executing COMMITTED pass command for vehicle " << vehicleId << std::endl;

    // If it's me, start moving
    if (vehicleId == myId_) {
        resumeMovement();
    }

    // If I'm still leader, continue coordinating
    if (isLeader_ && !hasPassedIntersection_) {
        timerManager.create(
            veins::TimerSpecification([this]() {
                if (isLeader_ && !hasPassedIntersection_ && activeVehicles_.size() > 0) {
                    sendStatusRequest();
                }
            }).oneshotIn(SimTime(0.05))
        );
    }
}

// ============ VEHICLE PASSED NOTIFICATION ============

void WillemtRaftApplication::sendVehiclePassed()
{
    std::cout << std::fixed << std::setprecision(1)
              << (simTime().dbl() * 1000.0) << "ms Vehicle " << myId_
              << " BROADCASTING vehicle-passed message" << std::endl;

    std::ostringstream packetName;
    packetName << "coord-vehicle-passed-from-" << myId_ << "-broadcast";

    std::vector<uint8_t> data = {static_cast<uint8_t>(myId_)};
    auto payload = makeShared<BytesChunk>(data);
    auto packet = createPacket(packetName.str());
    packet->insertAtBack(payload);
    sendPacket(std::move(packet));

    messagesSent_++;
}

void WillemtRaftApplication::handleVehiclePassed(int vehicleId)
{
    std::cout << std::fixed << std::setprecision(1)
              << (simTime().dbl() * 1000.0) << "ms Vehicle " << myId_
              << " RECEIVED vehicle-passed from vehicle " << vehicleId << std::endl;

    activeVehicles_.erase(vehicleId);

    if (vehicleInFrontOfMe_ == vehicleId) {
        wayOfSight_ = true;
        vehicleInFrontOfMe_ = -1;

        int vehiclesPerSide = totalVehicles_ / 4;
        if (vehiclesPerSide == 0) vehiclesPerSide = 1;
        int myPositionInLane = myId_ % vehiclesPerSide;
        int laneStartId = (myId_ / vehiclesPerSide) * vehiclesPerSide;

        for (int pos = 0; pos < myPositionInLane; pos++) {
            int otherVehicle = laneStartId + pos;
            if (activeVehicles_.count(otherVehicle) > 0) {
                vehicleInFrontOfMe_ = otherVehicle;
                wayOfSight_ = false;
                break;
            }
        }
    }
    
    // CRITICAL: If I'm the leader, continue coordinating after a vehicle passes
    if (isLeader_ && !hasPassedIntersection_ && activeVehicles_.size() > 0) {
        std::cout << std::fixed << std::setprecision(1)
                  << (simTime().dbl() * 1000.0) << "ms Leader " << myId_
                  << " scheduling next coordination round (" << activeVehicles_.size() 
                  << " vehicles remaining)" << std::endl;
        
        timerManager.create(
            veins::TimerSpecification([this]() {
                if (isLeader_ && !hasPassedIntersection_ && activeVehicles_.size() > 0) {
                    sendStatusRequest();
                }
            }).oneshotIn(SimTime(0.1))
        );
    }
}

// Continue to Part 5 (final)...
// WillemtRaftApplication.cc Part 5 (FINAL) - INTERSECTION COORDINATION AND METRICS

// ============ INTERSECTION DETECTION ============

void WillemtRaftApplication::checkAndStopAtIntersection()
{
    if (hasStoppedAtIntersection_ || !traciVehicle_) return;

    try {
        // Check if on approach edge
        std::string roadId = traciVehicle_->getRoadId();
        bool onApproachEdge = intersectionEdges_.count(roadId) > 0;
        
        if (!onApproachEdge) return;  // Not on approach edge yet
        
        // Get distance to junction
        double distToJunction = getDistanceToJunction();
        
        // Stop if within stopping distance
        if (distToJunction >= 0 && distToJunction <= intersectionStopDistance_) {
            stopVehicle();
            hasStoppedAtIntersection_ = true;
            timeStopped_ = simTime();
            intersectionEdge_ = roadId;
            calculateWayOfSight();
            
            std::cout << std::fixed << std::setprecision(1)
                      << (simTime().dbl() * 1000.0) << "ms Vehicle " << myId_
                      << " STOPPED at intersection (dist=" << std::setprecision(1) << distToJunction 
                      << "m, edge=" << roadId << ", isLeader=" << isLeader_ 
                      << ", hasCommittedOrder=" << hasCommittedOrder_ << ")" << std::endl;
            
            // If we're already the leader, wait for more vehicles then start collection
            if (isLeader_ && !hasCommittedOrder_) {
                std::cout << std::fixed << std::setprecision(1)
                          << (simTime().dbl() * 1000.0) << "ms Vehicle " << myId_
                          << " is LEADER - waiting " << arrivalWaitTimeMs_ << "ms for vehicles to arrive" << std::endl;
                
                // Wait for vehicles to gather before collecting status
                timerManager.create(
                    veins::TimerSpecification([this]() {
                        if (isLeader_ && !hasCommittedOrder_ && !hasPassedIntersection_) {
                            std::cout << std::fixed << std::setprecision(1)
                                      << (simTime().dbl() * 1000.0) << "ms Vehicle " << myId_
                                      << " arrival wait complete - starting status collection" << std::endl;
                            collectStatusAndDecide();
                        }
                    }).oneshotIn(SimTime(arrivalWaitTimeMs_, SIMTIME_MS))
                );
            }
        }
    } catch (...) {}
}

void WillemtRaftApplication::calculateWayOfSight()
{
    int vehiclesPerSide = totalVehicles_ / 4;
    if (vehiclesPerSide == 0) vehiclesPerSide = 1;
    int positionInLane = myId_ % vehiclesPerSide;

    if (positionInLane == 0) {
        wayOfSight_ = true;
        vehicleInFrontOfMe_ = -1;
    } else {
        int laneStartId = (myId_ / vehiclesPerSide) * vehiclesPerSide;
        vehicleInFrontOfMe_ = -1;
        wayOfSight_ = true;

        for (int pos = positionInLane - 1; pos >= 0; pos--) {
            int otherVehicle = laneStartId + pos;
            if (activeVehicles_.count(otherVehicle) > 0) {
                vehicleInFrontOfMe_ = otherVehicle;
                wayOfSight_ = false;
                break;
            }
        }
    }
}

bool WillemtRaftApplication::isAtIntersection() const
{
    if (!traciVehicle_) return false;
    try {
        std::string roadId = traciVehicle_->getRoadId();
        // Must be on an approach edge AND near the junction
        return intersectionEdges_.count(roadId) > 0 && isNearJunction();
    } catch (...) {
        return false;
    }
}

bool WillemtRaftApplication::isNearJunction() const
{
    double dist = getDistanceToJunction();
    return dist >= 0 && dist <= intersectionStopDistance_;
}

double WillemtRaftApplication::getDistanceToJunction() const
{
    if (!traciVehicle_) return -1;
    
    try {
        // Get lane length and current position on lane
        double lanePos = traciVehicle_->getLanePosition();
        std::string laneId = traciVehicle_->getLaneId();
        
        // Get lane length via TraCI
        double laneLength = traci_->lane(laneId).getLength();
        
        // Distance to end of lane (junction) = laneLength - currentPosition
        double distToEnd = laneLength - lanePos;
        
        return distToEnd;
    } catch (...) {
        return -1;
    }
}

bool WillemtRaftApplication::hasPassedIntersectionEdge() const
{
    if (!traciVehicle_ || intersectionEdge_.empty()) return false;
    try {
        std::string currentRoad = traciVehicle_->getRoadId();
        return currentRoad != intersectionEdge_;
    } catch (...) {
        return false;
    }
}

// ============ LEADERSHIP CHANGES ============

void WillemtRaftApplication::onBecameLeader()
{
    timeElected_ = simTime();
    wasElectedLeader_ = true;
    failedElectionCount_ = 0;

    double electionTimeMs = (timeElected_ - timeStopped_).dbl() * 1000.0;
    std::cout << std::fixed << std::setprecision(1)
              << (simTime().dbl() * 1000.0) << "ms Vehicle " << myId_
              << " ELECTED LEADER (election took " << electionTimeMs << "ms)" << std::endl;

    if (hasStoppedAtIntersection_ && !hasPassedIntersection_ && !hasCommittedOrder_) {
        // Wait for more vehicles to arrive before scheduling
        std::cout << std::fixed << std::setprecision(1)
                  << (simTime().dbl() * 1000.0) << "ms Leader " << myId_
                  << " waiting " << arrivalWaitTimeMs_ << "ms for vehicles to arrive" << std::endl;
        
        timerManager.create(
            veins::TimerSpecification([this]() {
                if (isLeader_ && !hasCommittedOrder_ && !hasPassedIntersection_) {
                    collectStatusAndDecide();
                }
            }).oneshotIn(SimTime(arrivalWaitTimeMs_, SIMTIME_MS))
        );
    }
}

void WillemtRaftApplication::onLostLeadership()
{
    std::cout << std::fixed << std::setprecision(1)
              << (simTime().dbl() * 1000.0) << "ms Vehicle " << myId_
              << " LOST LEADERSHIP (uncommitted proposals will not execute)" << std::endl;

    waitingForStatus_ = false;
    collectedWayOfSight_.clear();
}

// ============ VEHICLE MOVEMENT ============

void WillemtRaftApplication::resumeMovement()
{
    if (!traciVehicle_ || hasPassedIntersection_) return;

    try {
        // CRITICAL: Disable ALL SUMO safety checks for coordinated intersection
        // setSpeedMode(0) = ignore collisions and car-following model
        traciVehicle_->setSpeedMode(0);
        
        // Ignore junction priority rules
        traciVehicle_->setParameter("jmIgnoreFoeProb", "1.0");
        traciVehicle_->setParameter("jmIgnoreFoeSpeed", "100.0");
        traciVehicle_->setParameter("jmTimegapMinor", "0.0");
        
        // Set EXPLICIT speed (NOT -1, which re-enables car-following!)
        double speed = traciVehicle_->getMaxSpeed();
        if (speed <= 0) speed = 13.89;  // 50 km/h default
        traciVehicle_->setSpeed(speed);
        
        timeStartedMoving_ = simTime();

        std::cout << std::fixed << std::setprecision(1)
                  << (simTime().dbl() * 1000.0) << "ms Vehicle " << myId_
                  << " MOVING" << std::endl;

        // Start periodic polling to detect when vehicle leaves intersection
        // This simulates real-world sensors detecting vehicle exit
        timerManager.create(
            veins::TimerSpecification([this]() {
                checkIfLeftIntersection();
            }).interval(SimTime(0.1))  // Poll every 100ms
        );
    } catch (...) {}
}

void WillemtRaftApplication::checkIfLeftIntersection()
{
    if (hasPassedIntersection_ || !traciVehicle_) return;
    
    try {
        std::string currentRoad = traciVehicle_->getRoadId();
        
        // Dynamic exit edge detection using parsed edge configuration
        bool isExit = (exitEdges_.count(currentRoad) > 0);
        
        // Robust fallback: if vehicle has resumed and is no longer on
        // approach or internal edge, it has passed through
        if (!isExit && timeStartedMoving_ > SIMTIME_ZERO && !currentRoad.empty()) {
            bool onApproach = intersectionEdges_.count(currentRoad) > 0;
            bool onInternal = (currentRoad[0] == ':');
            if (!onApproach && !onInternal) {
                isExit = true;
            }
        }
        
        if (isExit) {
            // We've truly left the intersection!
            timePassed_ = simTime();
            double totalTimeMs = (timePassed_ - timeStopped_).dbl() * 1000.0;
            
            std::cout << std::fixed << std::setprecision(1)
                      << (simTime().dbl() * 1000.0) << "ms Vehicle " << myId_
                      << " PASSED (total wait " << totalTimeMs << "ms)" << std::endl;

            // CRITICAL: Broadcast BEFORE marking as passed
            sendVehicleLeft();  // Broadcast that I left
            activeVehicles_.erase(myId_);
            
            // Mark as passed and output metrics
            hasPassedIntersection_ = true;
            outputMetricsJSON();

            // IMPORTANT: Delay RAFT cleanup to allow broadcast to propagate
            timerManager.create(
                veins::TimerSpecification([this]() {
                    if (raftServer_) {
                        raft_free(raftServer_);
                        raftServer_ = nullptr;
                    }
                }).oneshotIn(SimTime(0.2))  // 200ms delay for message propagation
            );
        }
    } catch (...) {}
}

void WillemtRaftApplication::stopVehicle()
{
    if (!traciVehicle_) return;
    try {
        traciVehicle_->setSpeed(0);
    } catch (...) {}
}

void WillemtRaftApplication::handleFallback()
{
    if (isFallbackMode_ || hasPassedIntersection_) return;

    std::cout << std::fixed << std::setprecision(1)
              << (simTime().dbl() * 1000.0) << "ms Vehicle " << myId_
              << " entering FALLBACK mode" << std::endl;

    isFallbackMode_ = true;
    simtime_t fallbackStartTime = simTime();

    // INTELLIGENT FALLBACK: Check if we have a committed PASS_ORDER
    if (hasCommittedOrder_) {
        // We have the order! Follow it with estimated delays
        int myPosition = findMyPositionInOrder();
        
        if (myPosition >= 0) {
            std::cout << std::fixed << std::setprecision(1)
                      << (simTime().dbl() * 1000.0) << "ms Vehicle " << myId_
                      << " FALLBACK: Found myself at position " << myPosition 
                      << " in committed order, following it" << std::endl;
            
            // Estimate delay: 500ms per vehicle before us
            int estimatedDelayMs = myPosition * 500;
            
            timerManager.create(
                veins::TimerSpecification([this, fallbackStartTime]() {
                    if (!hasPassedIntersection_) {
                        coordinationMethod_ = "fallback";  // Mark as fallback since we didn't get vehicle-left messages
                        
                        // Record fallback time
                        simtime_t fallbackEndTime = simTime();
                        double fallbackTimeMs = (fallbackEndTime - fallbackStartTime).dbl() * 1000.0;
                        
                        std::cout << std::fixed << std::setprecision(1)
                                  << (simTime().dbl() * 1000.0) << "ms Vehicle " << myId_
                                  << " FALLBACK: Moving after " << fallbackTimeMs << "ms delay" << std::endl;
                        
                        resumeMovement();
                    }
                }).oneshotIn(SimTime(estimatedDelayMs / 1000.0))
            );
            return;
        }
    }
    
    // FINAL FALLBACK: No order or not in order, use random delay
    std::cout << std::fixed << std::setprecision(1)
              << (simTime().dbl() * 1000.0) << "ms Vehicle " << myId_
              << " FALLBACK: No committed order found, using random delay" << std::endl;
    
    coordinationMethod_ = "fallback";
    
    int waitMs = fallbackWaitMinMs_ +
                 (rand() % (fallbackWaitMaxMs_ - fallbackWaitMinMs_ + 1));

    timerManager.create(
        veins::TimerSpecification([this, fallbackStartTime]() {
            if (!hasPassedIntersection_) {
                simtime_t fallbackEndTime = simTime();
                double fallbackTimeMs = (fallbackEndTime - fallbackStartTime).dbl() * 1000.0;
                
                std::cout << std::fixed << std::setprecision(1)
                          << (simTime().dbl() * 1000.0) << "ms Vehicle " << myId_
                          << " FALLBACK: Moving after " << fallbackTimeMs << "ms random delay" << std::endl;
                
                resumeMovement();
            }
        }).oneshotIn(SimTime(waitMs / 1000.0))
    );
}

// ============ METRICS OUTPUT ============

void WillemtRaftApplication::outputMetricsJSON()
{
    if (!resultsFileOpened_) return;

    // Calculate timestamps in ms
    double arrivedMs = timeArrived_.dbl() * 1000.0;
    double stoppedMs = timeStopped_.dbl() * 1000.0;
    double clusterFormedMs = timeClusterFormed_.dbl() * 1000.0;
    double electedMs = timeElected_.dbl() * 1000.0;
    double orderCommittedMs = timeOrderCommitted_.dbl() * 1000.0;
    double startedMovingMs = timeStartedMoving_.dbl() * 1000.0;
    double passedMs = timePassed_.dbl() * 1000.0;
    
    // Calculate durations
    double approachTimeMs = stoppedMs - arrivedMs;
    double electionTimeMs = wasElectedLeader_ ? (electedMs - stoppedMs) : 0;
    
    // RAFT decision time: from max(stopped, cluster_formed) to order_committed
    double raftStartMs = std::max(stoppedMs, clusterFormedMs);
    double raftDecisionTimeMs = 0;
    if (orderCommittedMs > 0 && raftStartMs > 0 && orderCommittedMs >= raftStartMs) {
        raftDecisionTimeMs = orderCommittedMs - raftStartMs;
    }
    
    double totalWaitTimeMs = startedMovingMs - stoppedMs;
    double transitTimeMs = passedMs - startedMovingMs;
    double throughputVehPerSec = (passedMs > stoppedMs) ? 1.0 / ((passedMs - stoppedMs) / 1000.0) : 0;

    if (vehiclesCompleted_ > 0) {
        resultsFile_ << ",\n";
    }

    resultsFile_ << "  {\n"
                 << "    \"vehicle_id\": " << myId_ << ",\n"
                 << "    \"total_vehicles\": " << totalVehicles_ << ",\n"
                 << "    \"coordination_method\": \"" << coordinationMethod_ << "\",\n"
                 << "    \"was_leader\": " << (wasElectedLeader_ ? "true" : "false") << ",\n"
                 << "    \"way_of_sight\": " << (wayOfSight_ ? "true" : "false") << ",\n"
                 << "    \"lane\": \"" << myLane_ << "\",\n"
                 << "    \"batch_assigned\": " << myBatch_ << ",\n"
                 << "    \"log_entries_proposed\": " << logEntriesProposed_ << ",\n"
                 << "    \"log_entries_committed\": " << logEntriesCommitted_ << ",\n"
                 << "    \"timestamps_ms\": {\n"
                 << "      \"arrived\": " << std::fixed << std::setprecision(2) << arrivedMs << ",\n"
                 << "      \"stopped\": " << stoppedMs << ",\n"
                 << "      \"cluster_formed\": " << clusterFormedMs << ",\n"
                 << "      \"elected\": " << electedMs << ",\n"
                 << "      \"order_committed\": " << orderCommittedMs << ",\n"
                 << "      \"started_moving\": " << startedMovingMs << ",\n"
                 << "      \"passed\": " << passedMs << "\n"
                 << "    },\n"
                 << "    \"durations_ms\": {\n"
                 << "      \"approach_time\": " << approachTimeMs << ",\n"
                 << "      \"election_time\": " << electionTimeMs << ",\n"
                 << "      \"raft_decision_time\": " << raftDecisionTimeMs << ",\n"
                 << "      \"total_wait_time\": " << totalWaitTimeMs << ",\n"
                 << "      \"transit_time\": " << transitTimeMs << "\n"
                 << "    },\n"
                 << "    \"messages\": {\n"
                 << "      \"sent\": " << messagesSent_ << ",\n"
                 << "      \"received\": " << messagesReceived_ << "\n"
                 << "    },\n"
                 << "    \"election_rounds\": " << electionRounds_ << ",\n"
                 << "    \"throughput_veh_per_sec\": " << std::setprecision(4) << throughputVehPerSec << "\n"
                 << "  }";

    resultsFile_.flush();
    vehiclesCompleted_++;
    metricsWritten_ = true;

    if (vehiclesCompleted_ >= totalVehiclesStatic_) {
        closeResultsFile();
        std::cout << "All " << totalVehiclesStatic_ << " vehicles completed. Terminating simulation." << std::endl;
        endSimulation();
    }
}

// ============ UTILITY ============

raft_node_id_t WillemtRaftApplication::getNodeIdFromVehicleId(int vehicleId) const
{
    return vehicleId + 1;
}

int WillemtRaftApplication::getVehicleIdFromNodeId(raft_node_id_t nodeId) const
{
    return nodeId - 1;
}
