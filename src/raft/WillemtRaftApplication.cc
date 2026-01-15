#include "raft/WillemtRaftApplication.h"
#include <iostream>
#include <sstream>
#include <cstring>
#include <algorithm>
#include <cstddef>
#include <iomanip>
#include <random>
#include "inet/common/packet/Packet.h"
#include "inet/common/packet/chunk/BytesChunk.h"

extern "C" {
#include "../../third_party/raft/raft.h"
}

using namespace inet;

Define_Module(WillemtRaftApplication);

// Static member initialization
const std::set<std::string> WillemtRaftApplication::INTERSECTION_EDGES = {
    "N2C", "S2C", "E2C", "W2C",
    "C2S", "C2N", "C2E", "C2W"
};

// Route to incoming lane mapping
const std::map<std::string, std::string> WillemtRaftApplication::ROUTE_TO_LANE = {
    {"rN", "N2C"},
    {"rS", "S2C"},
    {"rE", "E2C"},
    {"rW", "W2C"}
};

std::ofstream WillemtRaftApplication::resultsFile_;
bool WillemtRaftApplication::resultsFileOpened_ = false;
int WillemtRaftApplication::vehiclesCompleted_ = 0;
int WillemtRaftApplication::totalVehiclesStatic_ = 4;
std::string WillemtRaftApplication::resultsFileNameStatic_ = "raft_results.json";

WillemtRaftApplication::WillemtRaftApplication()
    : raftServer_(nullptr)
    , myId_(-1)
    , myRaftNodeId_(-1)
    , mobility_(nullptr)
    , traci_(nullptr)
    , traciVehicle_(nullptr)
    , isLeader_(false)
    , hasStoppedAtIntersection_(false)
    , hasPassedIntersection_(false)
    , wayOfSight_(true)
    , waitingForStatus_(false)
    , statusResponseCount_(0)
    , vehicleInFrontOfMe_(-1)
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
    , timeArrived_(0)
    , timeStopped_(0)
    , timeElected_(0)
    , timeStartedMoving_(0)
    , timePassed_(0)
    , messagesSent_(0)
    , messagesReceived_(0)
    , electionRounds_(0)
    , wasElectedLeader_(false)
    , coordinationMethod_("raft")
{
}

WillemtRaftApplication::~WillemtRaftApplication()
{
    if (raftServer_) {
        raft_free(raftServer_);
        raftServer_ = nullptr;
    }
}

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

bool WillemtRaftApplication::startApplication()
{
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
    statusCollectionTimeoutMs_ = par("statusCollectionTimeoutMs").intValue();
    resultsFileName_ = par("resultsFile").stdstringValue();
    
    // Calculate vehicle's lane based on vehicle ID pattern
    // Vehicle assignment: N,N,S,S,E,E,W,W for 8 vehicles
    // For 16: N,N,N,N,S,S,S,S,E,E,E,E,W,W,W,W
    int vehiclesPerSide = totalVehicles_ / 4;
    if (vehiclesPerSide == 0) vehiclesPerSide = 1;
    
    int sideIndex = myId_ / vehiclesPerSide;
    int positionInLane = myId_ % vehiclesPerSide;
    
    switch (sideIndex % 4) {
        case 0: myLane_ = "N2C"; myRoute_ = "rN"; break;
        case 1: myLane_ = "S2C"; myRoute_ = "rS"; break;
        case 2: myLane_ = "E2C"; myRoute_ = "rE"; break;
        case 3: myLane_ = "W2C"; myRoute_ = "rW"; break;
    }
    
    // Determine which vehicle is in front of me
    // Vehicle in front has same lane but lower ID within lane group
    if (positionInLane > 0) {
        vehicleInFrontOfMe_ = myId_ - 1;  // Previous vehicle in same lane
        wayOfSight_ = false;  // Initially blocked
    } else {
        vehicleInFrontOfMe_ = -1;  // I'm at front
        wayOfSight_ = true;  // Clear view
    }
    
    // Initialize active vehicles set
    for (int i = 0; i < totalVehicles_; i++) {
        activeVehicles_.insert(i);
    }
    
    // Update static total for results file
    totalVehiclesStatic_ = totalVehicles_;
    
    // Open results file on first vehicle
    if (myId_ == 0) {
        openResultsFile(resultsFileName_);
    }
    
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
    
    // Initialize RAFT server
    raftServer_ = raft_new();
    if (!raftServer_) {
        EV_ERROR << "Vehicle " << myId_ << " ERROR: Could not create RAFT server!" << endl;
        return false;
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
    
    // Set timeouts from parameters
    int electionTimeout = electionTimeoutBaseMs_ + (myId_ * electionTimeoutJitterMs_);
    raft_set_election_timeout(raftServer_, electionTimeout);
    raft_set_request_timeout(raftServer_, requestTimeoutMs_);
    
    // Add all nodes to the cluster
    for (int i = 0; i < totalVehicles_; i++) {
        int nodeId = i + 1;
        void* userData = reinterpret_cast<void*>(static_cast<intptr_t>(i));
        int isSelf = (i == myId_) ? 1 : 0;
        raft_add_node(raftServer_, userData, nodeId, isSelf);
    }
    
    timeArrived_ = simTime();
    
    // Schedule periodic position check
    timerManager.create(
        veins::TimerSpecification([this]() {
            checkAndStopAtIntersection();
        }).interval(SimTime(CHECK_INTERVAL))
    );
    
    // Schedule RAFT periodic processing
    timerManager.create(
        veins::TimerSpecification([this]() {
            processRaftPeriodic();
        }).interval(SimTime(RAFT_PERIODIC_INTERVAL))
    );
    
    return true;
}

bool WillemtRaftApplication::stopApplication()
{
    if (raftServer_) {
        raft_free(raftServer_);
        raftServer_ = nullptr;
    }
    return true;
}

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
                if (failedElectionCount_ >= maxFailedElections_ && hasStoppedAtIntersection_) {
                    isFallbackMode_ = true;
                    coordinationMethod_ = "fallback";
                    handleFallback();
                    return;
                }
            } else {
                failedElectionCount_ = 0;  // Reset on successful election
            }
            lastCheckedTerm_ = currentTerm;
        }
        
        // Check if we became leader
        bool wasLeader = isLeader_;
        isLeader_ = (raft_is_leader(raftServer_) == 1);
        
        if (isLeader_ && !wasLeader) {
            onBecameLeader();
        } else if (!isLeader_ && wasLeader) {
            onLostLeadership();
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
    return 0;
}

int WillemtRaftApplication::applylog(raft_server_t* raft, void* user_data,
                                    raft_entry_t* entry, raft_index_t entry_idx)
{
    return 0;
}

void WillemtRaftApplication::log(raft_server_t* raft, raft_node_t* node,
                                 void* user_data, const char* buf)
{
}

int WillemtRaftApplication::persistVote(raft_server_t* raft, void* user_data, raft_node_id_t vote)
{
    return 0;
}

int WillemtRaftApplication::doSendRequestVote(raft_node_t* node, msg_requestvote_t* msg)
{
    if (!node) return -1;
    
    void* userData = raft_node_get_udata(node);
    int targetVehicleId = static_cast<int>(reinterpret_cast<intptr_t>(userData));
    
    // Only send to active vehicles
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
    
    // Only send to active vehicles
    if (activeVehicles_.find(targetVehicleId) == activeVehicles_.end()) {
        return 0;
    }
    
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

// ============ PACKET PROCESSING ============

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

void WillemtRaftApplication::processPacket(std::shared_ptr<Packet> pk)
{
    if (hasPassedIntersection_) return;
    
    std::string packetName = pk->getName();
    
    // Check if this packet is for me
    int targetId = extractTargetFromPacketName(packetName);
    bool isBroadcast = (packetName.find("-broadcast") != std::string::npos);
    
    if (!isBroadcast && targetId != -1 && targetId != myId_) {
        return;  // Not for me
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
    // Handle custom coordination messages
    else if (packetName.find("coord-status-request") != std::string::npos) {
        int fromLeader = extractSenderFromPacketName(packetName);
        handleStatusRequest(fromLeader);
    } else if (packetName.find("coord-status-response") != std::string::npos) {
        int fromVehicle = extractSenderFromPacketName(packetName);
        // Extract wayOfSight from packet data
        bool wos = (bytes.size() > 0 && bytes[0] == 1);
        handleStatusResponse(fromVehicle, wos);
    } else if (packetName.find("coord-pass-command") != std::string::npos) {
        int fromLeader = extractSenderFromPacketName(packetName);
        handlePassCommand(fromLeader);
    } else if (packetName.find("coord-vehicle-passed") != std::string::npos) {
        int vehicleId = extractSenderFromPacketName(packetName);
        handleVehiclePassed(vehicleId);
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
    size_t baseSize = offsetof(msg_appendentries_t, entries);
    std::vector<uint8_t> data(baseSize);
    memcpy(data.data(), msg, baseSize);
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
    if (data.size() >= baseSize) {
        memcpy(msg, data.data(), baseSize);
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

// ============ NEW COORDINATION PROTOCOL ============

void WillemtRaftApplication::sendStatusRequest()
{
    // Leader broadcasts status request to all active vehicles
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
    
    // Set timeout for status collection
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
    
    // Respond with my wayOfSight status
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
    
    // Check if we have responses from all active vehicles
    int expectedResponses = activeVehicles_.size();
    if (statusResponseCount_ >= expectedResponses) {
        collectStatusAndDecide();
    }
}

void WillemtRaftApplication::collectStatusAndDecide()
{
    if (!isLeader_ || hasPassedIntersection_) return;
    
    waitingForStatus_ = false;
    
    // Find vehicles with wayOfSight = true
    int selectedVehicle = selectVehicleToPass();
    
    if (selectedVehicle >= 0) {
        // If leader is the only one left with wayOfSight, pass self
        // Otherwise, prefer to let others pass first (unless leader is only option)
        std::vector<int> eligibleOthers;
        int vehiclesPerSide = totalVehicles_ / 4;
        if (vehiclesPerSide == 0) vehiclesPerSide = 1;
        
        for (int vid : activeVehicles_) {
            if (vid == myId_) continue;
            
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
                eligibleOthers.push_back(vid);
            }
        }
        
        // If there are other eligible vehicles, prefer them over self
        if (!eligibleOthers.empty()) {
            std::random_device rd;
            std::mt19937 gen(rd());
            std::uniform_int_distribution<> dis(0, eligibleOthers.size() - 1);
            selectedVehicle = eligibleOthers[dis(gen)];
        }
        
        sendPassCommand(selectedVehicle);
    } else {
        // No vehicle can pass - schedule retry
        timerManager.create(
            veins::TimerSpecification([this]() {
                if (isLeader_ && !hasPassedIntersection_) {
                    sendStatusRequest();
                }
            }).oneshotIn(SimTime(0.1))
        );
    }
}

int WillemtRaftApplication::selectVehicleToPass()
{
    std::vector<int> canPassVehicles;
    int vehiclesPerSide = totalVehicles_ / 4;
    if (vehiclesPerSide == 0) vehiclesPerSide = 1;
    
    // First, add vehicles that reported wayOfSight=true
    for (const auto& entry : collectedWayOfSight_) {
        if (entry.second && activeVehicles_.count(entry.first) > 0) {
            canPassVehicles.push_back(entry.first);
        }
    }
    
    // Then, calculate which vehicles SHOULD have wayOfSight based on passed vehicles
    // This is more reliable than waiting for status responses
    for (int vid : activeVehicles_) {
        int sideIndex = vid / vehiclesPerSide;
        int positionInLane = vid % vehiclesPerSide;
        int laneStartId = sideIndex * vehiclesPerSide;
        
        bool hasWayOfSight = true;
        
        // Check if any vehicle in front of me on same lane is still active
        for (int pos = 0; pos < positionInLane; pos++) {
            int vehicleInFront = laneStartId + pos;
            if (activeVehicles_.count(vehicleInFront) > 0) {
                hasWayOfSight = false;
                break;
            }
        }
        
        if (hasWayOfSight) {
            canPassVehicles.push_back(vid);
        }
    }
    
    // Remove duplicates
    std::sort(canPassVehicles.begin(), canPassVehicles.end());
    canPassVehicles.erase(std::unique(canPassVehicles.begin(), canPassVehicles.end()), 
                          canPassVehicles.end());
    
    if (canPassVehicles.empty()) {
        return -1;
    }
    
    // Randomly select one vehicle
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, canPassVehicles.size() - 1);
    
    return canPassVehicles[dis(gen)];
}

void WillemtRaftApplication::sendPassCommand(int toVehicle)
{
    std::cout << std::fixed << std::setprecision(1)
              << (simTime().dbl() * 1000.0) << "ms Leader " << myId_
              << " COMMANDS Vehicle " << toVehicle << " to PASS" << std::endl;
    
    std::ostringstream packetName;
    packetName << "coord-pass-command-from-" << myId_ << "-to-" << toVehicle;
    
    std::vector<uint8_t> data = {static_cast<uint8_t>(toVehicle)};
    auto payload = makeShared<BytesChunk>(data);
    auto packet = createPacket(packetName.str());
    packet->insertAtBack(payload);
    sendPacket(std::move(packet));
    
    messagesSent_++;
    
    // If commanding self, move
    if (toVehicle == myId_) {
        resumeMovement();
    }
}

void WillemtRaftApplication::handlePassCommand(int fromLeader)
{
    if (hasPassedIntersection_) return;
    
    std::cout << std::fixed << std::setprecision(1)
              << (simTime().dbl() * 1000.0) << "ms Vehicle " << myId_
              << " RECEIVED pass command from Leader " << fromLeader << std::endl;
    
    resumeMovement();
}

void WillemtRaftApplication::sendVehiclePassed()
{
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
    // Remove from active vehicles
    activeVehicles_.erase(vehicleId);
    
    // Update wayOfSight - if the passed vehicle was in front of me, I now have clear view
    if (vehicleInFrontOfMe_ == vehicleId) {
        wayOfSight_ = true;
        vehicleInFrontOfMe_ = -1;
        
        // Check if there's another vehicle in front on my lane
        int vehiclesPerSide = totalVehicles_ / 4;
        if (vehiclesPerSide == 0) vehiclesPerSide = 1;
        int myPositionInLane = myId_ % vehiclesPerSide;
        int laneStartId = (myId_ / vehiclesPerSide) * vehiclesPerSide;
        
        // Look for any remaining vehicle in front of me on same lane
        for (int pos = 0; pos < myPositionInLane; pos++) {
            int otherVehicle = laneStartId + pos;
            if (activeVehicles_.count(otherVehicle) > 0) {
                vehicleInFrontOfMe_ = otherVehicle;
                wayOfSight_ = false;
                break;
            }
        }
    }
    
    // If I'm leader and not passed, continue coordinating
    if (isLeader_ && !hasPassedIntersection_ && !waitingForStatus_) {
        // Small delay before next status request
        timerManager.create(
            veins::TimerSpecification([this]() {
                if (isLeader_ && !hasPassedIntersection_ && activeVehicles_.size() > 0) {
                    sendStatusRequest();
                }
            }).oneshotIn(SimTime(0.05))
        );
    }
}

// ============ INTERSECTION COORDINATION ============

void WillemtRaftApplication::checkAndStopAtIntersection()
{
    if (hasStoppedAtIntersection_ || !traciVehicle_) return;
    
    try {
        if (isAtIntersection() && !hasStoppedAtIntersection_) {
            stopVehicle();
            hasStoppedAtIntersection_ = true;
            timeStopped_ = simTime();
            intersectionEdge_ = traciVehicle_->getRoadId();
            
            // Calculate wayOfSight based on vehicle position
            calculateWayOfSight();
        }
    } catch (...) {}
}

void WillemtRaftApplication::calculateWayOfSight()
{
    // Already calculated in startApplication based on vehicle ID pattern
    // This is called when we stop - just verify/update
    
    int vehiclesPerSide = totalVehicles_ / 4;
    if (vehiclesPerSide == 0) vehiclesPerSide = 1;
    int positionInLane = myId_ % vehiclesPerSide;
    
    if (positionInLane == 0) {
        wayOfSight_ = true;
        vehicleInFrontOfMe_ = -1;
    } else {
        // Check if vehicle in front is still active
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
        return INTERSECTION_EDGES.count(roadId) > 0;
    } catch (...) {
        return false;
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

void WillemtRaftApplication::onBecameLeader()
{
    timeElected_ = simTime();
    wasElectedLeader_ = true;
    failedElectionCount_ = 0;
    
    double electionTimeMs = (timeElected_ - timeStopped_).dbl() * 1000.0;
    std::cout << std::fixed << std::setprecision(1)
              << (simTime().dbl() * 1000.0) << "ms Vehicle " << myId_
              << " ELECTED LEADER (election took " << electionTimeMs << "ms)" << std::endl;
    
    // Start coordination - request status from all vehicles
    if (hasStoppedAtIntersection_ && !hasPassedIntersection_) {
        sendStatusRequest();
    }
}

void WillemtRaftApplication::onLostLeadership()
{
    // Lost leadership - maybe due to network partition or new term
    // Reset coordination state
    waitingForStatus_ = false;
    collectedWayOfSight_.clear();
}

void WillemtRaftApplication::resumeMovement()
{
    if (!traciVehicle_ || hasPassedIntersection_) return;
    
    try {
        traciVehicle_->setSpeed(-1);
        timeStartedMoving_ = simTime();
        
        std::cout << std::fixed << std::setprecision(1)
                  << (simTime().dbl() * 1000.0) << "ms Vehicle " << myId_
                  << " MOVING" << std::endl;
        
        // Timer to confirm passage
        timerManager.create(
            veins::TimerSpecification([this]() {
                if (!hasPassedIntersection_) {
                    hasPassedIntersection_ = true;
                    timePassed_ = simTime();
                    
                    double totalTimeMs = (timePassed_ - timeStopped_).dbl() * 1000.0;
                    std::cout << std::fixed << std::setprecision(1)
                              << (simTime().dbl() * 1000.0) << "ms Vehicle " << myId_
                              << " PASSED (total wait " << totalTimeMs << "ms)" << std::endl;
                    
                    // Notify others that I passed
                    sendVehiclePassed();
                    
                    // Remove self from active
                    activeVehicles_.erase(myId_);
                    
                    // Output metrics
                    outputMetricsJSON();
                    
                    // If I was leader, free RAFT so new election can happen
                    if (raftServer_) {
                        raft_free(raftServer_);
                        raftServer_ = nullptr;
                    }
                }
            }).oneshotIn(SimTime(passConfirmationMs_ / 1000.0))
        );
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
    double waitTimeMs = fallbackWaitMinMs_ + uniform(0, fallbackWaitMaxMs_ - fallbackWaitMinMs_);
    
    if (raftServer_) {
        raft_free(raftServer_);
        raftServer_ = nullptr;
    }
    
    std::cout << std::fixed << std::setprecision(1)
              << (simTime().dbl() * 1000.0) << "ms Vehicle " << myId_
              << " FALLBACK (waiting " << waitTimeMs << "ms)" << std::endl;
    
    timerManager.create(
        veins::TimerSpecification([this]() {
            if (!hasPassedIntersection_) {
                resumeMovement();
            }
        }).oneshotIn(SimTime(waitTimeMs / 1000.0))
    );
}

// ============ METRICS OUTPUT ============

void WillemtRaftApplication::outputMetricsJSON()
{
    if (!resultsFileOpened_) return;
    
    // Calculate metrics
    double stoppedTimeMs = (timeStopped_ - timeArrived_).dbl() * 1000.0;
    double electionTimeMs = wasElectedLeader_ ? (timeElected_ - timeStopped_).dbl() * 1000.0 : 0;
    double totalWaitTimeMs = (timeStartedMoving_ - timeStopped_).dbl() * 1000.0;
    double transitTimeMs = (timePassed_ - timeStartedMoving_).dbl() * 1000.0;
    double throughputVehPerSec = 1.0 / ((timePassed_ - timeStopped_).dbl());
    
    // Add comma for subsequent entries
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
                 << "    \"timestamps_ms\": {\n"
                 << "      \"arrived\": " << std::fixed << std::setprecision(2) << (timeArrived_.dbl() * 1000.0) << ",\n"
                 << "      \"stopped\": " << (timeStopped_.dbl() * 1000.0) << ",\n"
                 << "      \"elected\": " << (timeElected_.dbl() * 1000.0) << ",\n"
                 << "      \"started_moving\": " << (timeStartedMoving_.dbl() * 1000.0) << ",\n"
                 << "      \"passed\": " << (timePassed_.dbl() * 1000.0) << "\n"
                 << "    },\n"
                 << "    \"durations_ms\": {\n"
                 << "      \"approach_time\": " << stoppedTimeMs << ",\n"
                 << "      \"election_time\": " << electionTimeMs << ",\n"
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
    
    // Close file after all vehicles complete
    if (vehiclesCompleted_ >= totalVehiclesStatic_) {
        closeResultsFile();
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

void WillemtRaftApplication::sendCustomMessage(const std::string& type, const std::string& data)
{
    // Helper for sending custom coordination messages
    std::ostringstream packetName;
    packetName << type << "-from-" << myId_ << "-broadcast";
    
    std::vector<uint8_t> bytes(data.begin(), data.end());
    auto payload = makeShared<BytesChunk>(bytes);
    auto packet = createPacket(packetName.str());
    packet->insertAtBack(payload);
    sendPacket(std::move(packet));
    
    messagesSent_++;
}
