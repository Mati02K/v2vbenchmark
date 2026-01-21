#include "raft/GreedyRaftApplication.h"
#include <iostream>
#include <sstream>
#include <cstring>
#include <algorithm>
#include <cstddef>
#include <iomanip>
#include <random>
#include <atomic>
#include "inet/common/packet/Packet.h"
#include "inet/common/packet/chunk/BytesChunk.h"

extern "C" {
#include "../../third_party/raft/raft.h"
}

using namespace inet;

Define_Module(GreedyRaftApplication);

// Static member initialization
const std::set<std::string> GreedyRaftApplication::INTERSECTION_EDGES = {
    "N2C", "S2C", "E2C", "W2C",
    "C2S", "C2N", "C2E", "C2W"
};

const std::map<std::string, std::string> GreedyRaftApplication::ROUTE_TO_LANE = {
    {"rN", "N2C"},
    {"rS", "S2C"},
    {"rE", "E2C"},
    {"rW", "W2C"}
};

std::ofstream GreedyRaftApplication::resultsFile_;
bool GreedyRaftApplication::resultsFileOpened_ = false;
int GreedyRaftApplication::vehiclesCompleted_ = 0;
int GreedyRaftApplication::totalVehiclesStatic_ = 4;
std::string GreedyRaftApplication::resultsFileNameStatic_ = "greedy_raft_results.json";

GreedyRaftApplication::GreedyRaftApplication()
    : raftServer_(nullptr)
    , myId_(-1)
    , myRaftNodeId_(-1)
    , mySideIndex_(0)
    , myPositionInLane_(0)
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
    , vehiclesPerSide_(1)
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
    , coordinationMethod_("greedy_raft")
{
}

GreedyRaftApplication::~GreedyRaftApplication()
{
    if (raftServer_) {
        raft_free(raftServer_);
        raftServer_ = nullptr;
    }
}

void GreedyRaftApplication::openResultsFile(const std::string& filename)
{
    if (!resultsFileOpened_) {
        resultsFile_.open(filename);
        resultsFile_ << "[\n";
        resultsFileOpened_ = true;
        resultsFileNameStatic_ = filename;
    }
}

void GreedyRaftApplication::closeResultsFile()
{
    if (resultsFileOpened_) {
        resultsFile_ << "\n]";
        resultsFile_.close();
        resultsFileOpened_ = false;
        vehiclesCompleted_ = 0;
    }
}

bool GreedyRaftApplication::startApplication()
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
    
    // Calculate vehicles per side and my position
    vehiclesPerSide_ = totalVehicles_ / 4;
    if (vehiclesPerSide_ == 0) vehiclesPerSide_ = 1;
    
    mySideIndex_ = myId_ / vehiclesPerSide_;
    myPositionInLane_ = myId_ % vehiclesPerSide_;
    
    switch (mySideIndex_ % 4) {
        case 0: myLane_ = "N2C"; myRoute_ = "rN"; break;
        case 1: myLane_ = "S2C"; myRoute_ = "rS"; break;
        case 2: myLane_ = "E2C"; myRoute_ = "rE"; break;
        case 3: myLane_ = "W2C"; myRoute_ = "rW"; break;
    }
    
    // Determine which vehicle is in front of me
    if (myPositionInLane_ > 0) {
        vehicleInFrontOfMe_ = myId_ - 1;
        wayOfSight_ = false;
    } else {
        vehicleInFrontOfMe_ = -1;
        wayOfSight_ = true;
    }
    
    // Initialize active vehicles set
    for (int i = 0; i < totalVehicles_; i++) {
        activeVehicles_.insert(i);
    }
    
    totalVehiclesStatic_ = totalVehicles_;
    
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
    
    // Initial election timeout: base + small ID-based spread + jitter
    // Lower IDs get slightly shorter timeouts, giving them priority
    int idSpread = myId_ * 20;  // 20ms per ID = moderate spread
    int randomJitter = rand() % electionTimeoutJitterMs_;
    int electionTimeout = electionTimeoutBaseMs_ + idSpread + randomJitter;
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

bool GreedyRaftApplication::stopApplication()
{
    if (raftServer_) {
        raft_free(raftServer_);
        raftServer_ = nullptr;
    }
    
    // Ensure JSON file is properly closed on simulation end
    // Check if this is the last vehicle to stop (using static counter)
    static std::atomic<int> stoppedCount{0};
    int myStopNum = ++stoppedCount;
    
    // Last vehicle closes the file
    if (myStopNum >= totalVehicles_) {
        if (resultsFileOpened_) {
            closeResultsFile();
        }
        stoppedCount = 0;  // Reset for next simulation run
    }
    
    return true;
}

// ============ LANE HELPER FUNCTIONS ============

int GreedyRaftApplication::getLaneStartId(int vehicleId) const
{
    int sideIndex = vehicleId / vehiclesPerSide_;
    return sideIndex * vehiclesPerSide_;
}

int GreedyRaftApplication::getSideIndex(int vehicleId) const
{
    return vehicleId / vehiclesPerSide_;
}

int GreedyRaftApplication::getPositionInLane(int vehicleId) const
{
    return vehicleId % vehiclesPerSide_;
}

std::vector<int> GreedyRaftApplication::getLaneMates(int vehicleId) const
{
    std::vector<int> laneMates;
    int laneStart = getLaneStartId(vehicleId);
    for (int i = 0; i < vehiclesPerSide_; i++) {
        int mate = laneStart + i;
        if (mate < totalVehicles_) {
            laneMates.push_back(mate);
        }
    }
    return laneMates;
}

bool GreedyRaftApplication::isVehicleInMyLane(int vehicleId) const
{
    return getSideIndex(vehicleId) == mySideIndex_;
}

bool GreedyRaftApplication::canVehiclePass(int vehicleId) const
{
    // A vehicle can pass if no vehicle in front of it on same lane is still active
    int laneStart = getLaneStartId(vehicleId);
    int posInLane = getPositionInLane(vehicleId);
    
    for (int pos = 0; pos < posInLane; pos++) {
        int vehicleInFront = laneStart + pos;
        if (activeVehicles_.count(vehicleInFront) > 0) {
            return false;
        }
    }
    return true;
}

void GreedyRaftApplication::processRaftPeriodic()
{
    // Simple: stop RAFT processing after passing or in fallback
    if (!raftServer_ || hasPassedIntersection_ || isFallbackMode_) return;
    
    static std::map<int, simtime_t> lastTimeMap;
    static std::map<int, int> lastActiveCount;
    simtime_t now = simTime();
    simtime_t delta = now - lastTimeMap[myId_];
    lastTimeMap[myId_] = now;
    
    int msecElapsed = static_cast<int>(delta.dbl() * 1000.0);
    if (msecElapsed > 0) {
        raft_periodic(raftServer_, msecElapsed);
        
        // RESET BACKOFF when competition reduces (vehicles pass)
        int currentActiveCount = activeVehicles_.size();
        if (lastActiveCount.count(myId_) && currentActiveCount < lastActiveCount[myId_]) {
            // Someone passed! Reset our backoff to compete fresh
            failedElectionCount_ = std::max(0, failedElectionCount_ - 3);  // Reduce but don't fully reset
            
            // New competitive timeout: small base + random jitter
            int newTimeout = electionTimeoutBaseMs_ + (rand() % (electionTimeoutJitterMs_ * 2));
            raft_set_election_timeout(raftServer_, newTimeout);
            
            std::cout << (simTime().dbl() * 1000.0) << "ms Vehicle " << myId_ 
                      << " competition reduced (" << lastActiveCount[myId_] << "->" << currentActiveCount
                      << "), reset timeout=" << newTimeout << "ms" << std::endl;
        }
        lastActiveCount[myId_] = currentActiveCount;
        
        raft_term_t currentTerm = raft_get_current_term(raftServer_);
        if (currentTerm > lastCheckedTerm_) {
            electionRounds_++;
            if (!raft_is_leader(raftServer_)) {
                failedElectionCount_++;
                
                // EXPONENTIAL BACKOFF: quick escalation, fast fallback
                // Max 4x backoff, then fallback kicks in
                int backoffFactor = std::min(1 << std::min(failedElectionCount_, 2), 4);  // Max 4x
                int idSpread = myId_ * 15;  // Small ID-based spread
                int randomJitter = rand() % (electionTimeoutJitterMs_ * backoffFactor);
                int newTimeout = electionTimeoutBaseMs_ * backoffFactor + idSpread + randomJitter;
                
                raft_set_election_timeout(raftServer_, newTimeout);
                
                if (failedElectionCount_ >= maxFailedElections_) {
                    isFallbackMode_ = true;
                    coordinationMethod_ = "fallback";
                    handleFallback();
                    return;
                }
            } else {
                // Won election - reset backoff
                failedElectionCount_ = 0;
                raft_set_election_timeout(raftServer_, electionTimeoutBaseMs_ + (rand() % electionTimeoutJitterMs_));
            }
            lastCheckedTerm_ = currentTerm;
        }
        
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

int GreedyRaftApplication::sendRequestVote(raft_server_t* raft, void* user_data,
                                           raft_node_t* node, msg_requestvote_t* msg)
{
    GreedyRaftApplication* app = static_cast<GreedyRaftApplication*>(user_data);
    return app->doSendRequestVote(node, msg);
}

int GreedyRaftApplication::sendAppendEntries(raft_server_t* raft, void* user_data,
                                             raft_node_t* node, msg_appendentries_t* msg)
{
    GreedyRaftApplication* app = static_cast<GreedyRaftApplication*>(user_data);
    return app->doSendAppendEntries(node, msg);
}

int GreedyRaftApplication::logOffer(raft_server_t* raft, void* user_data,
                                    raft_entry_t* entry, raft_index_t entry_idx)
{
    return 0;
}

int GreedyRaftApplication::applylog(raft_server_t* raft, void* user_data,
                                   raft_entry_t* entry, raft_index_t entry_idx)
{
    return 0;
}

void GreedyRaftApplication::log(raft_server_t* raft, raft_node_t* node,
                                void* user_data, const char* buf)
{
}

int GreedyRaftApplication::persistVote(raft_server_t* raft, void* user_data, raft_node_id_t vote)
{
    return 0;
}

int GreedyRaftApplication::doSendRequestVote(raft_node_t* node, msg_requestvote_t* msg)
{
    if (!node) return -1;
    
    void* userData = raft_node_get_udata(node);
    int targetVehicleId = static_cast<int>(reinterpret_cast<intptr_t>(userData));
    
    if (activeVehicles_.find(targetVehicleId) == activeVehicles_.end()) {
        return 0;
    }
    
    std::vector<uint8_t> data = serializeRequestVote(msg);
    
    std::ostringstream packetName;
    packetName << "greedy-raft-requestvote-from-" << myId_ << "-to-" << targetVehicleId;
    
    auto payload = makeShared<BytesChunk>(data);
    auto packet = createPacket(packetName.str());
    packet->insertAtBack(payload);
    sendPacket(std::move(packet));
    
    messagesSent_++;
    return 0;
}

int GreedyRaftApplication::doSendAppendEntries(raft_node_t* node, msg_appendentries_t* msg)
{
    if (!node) return -1;
    
    void* userData = raft_node_get_udata(node);
    int targetVehicleId = static_cast<int>(reinterpret_cast<intptr_t>(userData));
    
    if (activeVehicles_.find(targetVehicleId) == activeVehicles_.end()) {
        return 0;
    }
    
    std::vector<uint8_t> data = serializeAppendEntries(msg);
    
    std::ostringstream packetName;
    packetName << "greedy-raft-appendentries-from-" << myId_ << "-to-" << targetVehicleId;
    
    auto payload = makeShared<BytesChunk>(data);
    auto packet = createPacket(packetName.str());
    packet->insertAtBack(payload);
    sendPacket(std::move(packet));
    
    messagesSent_++;
    return 0;
}

// ============ PACKET PROCESSING ============

int GreedyRaftApplication::extractTargetFromPacketName(const std::string& packetName)
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

int GreedyRaftApplication::extractSenderFromPacketName(const std::string& packetName)
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

void GreedyRaftApplication::processPacket(std::shared_ptr<Packet> pk)
{
    // Simple: stop processing after passing
    if (hasPassedIntersection_) return;
    
    std::string packetName = pk->getName();
    
    // Only process greedy-raft messages
    if (packetName.find("greedy-") == std::string::npos) {
        return;
    }
    
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
    if (packetName.find("greedy-raft-requestvote-response") != std::string::npos) {
        handleRequestVoteResponse(bytes, packetName);
    } else if (packetName.find("greedy-raft-requestvote") != std::string::npos) {
        handleRequestVote(bytes, packetName);
    } else if (packetName.find("greedy-raft-appendentries-response") != std::string::npos) {
        handleAppendEntriesResponse(bytes, packetName);
    } else if (packetName.find("greedy-raft-appendentries") != std::string::npos) {
        handleAppendEntries(bytes, packetName);
    }
    // Handle custom coordination messages
    else if (packetName.find("greedy-coord-status-request") != std::string::npos) {
        int fromLeader = extractSenderFromPacketName(packetName);
        handleStatusRequest(fromLeader);
    } else if (packetName.find("greedy-coord-status-response") != std::string::npos) {
        int fromVehicle = extractSenderFromPacketName(packetName);
        bool wos = (bytes.size() > 0 && bytes[0] == 1);
        handleStatusResponse(fromVehicle, wos);
    } else if (packetName.find("greedy-coord-pass-command") != std::string::npos) {
        int fromLeader = extractSenderFromPacketName(packetName);
        handlePassCommand(fromLeader);
    } else if (packetName.find("greedy-coord-vehicle-passed") != std::string::npos) {
        int vehicleId = extractSenderFromPacketName(packetName);
        handleVehiclePassed(vehicleId);
    }
}

// ============ SERIALIZATION ============

std::vector<uint8_t> GreedyRaftApplication::serializeRequestVote(msg_requestvote_t* msg)
{
    std::vector<uint8_t> data(sizeof(*msg));
    memcpy(data.data(), msg, sizeof(*msg));
    return data;
}

std::vector<uint8_t> GreedyRaftApplication::serializeRequestVoteResponse(msg_requestvote_response_t* msg)
{
    std::vector<uint8_t> data(sizeof(*msg));
    memcpy(data.data(), msg, sizeof(*msg));
    return data;
}

std::vector<uint8_t> GreedyRaftApplication::serializeAppendEntries(msg_appendentries_t* msg)
{
    size_t baseSize = offsetof(msg_appendentries_t, entries);
    std::vector<uint8_t> data(baseSize);
    memcpy(data.data(), msg, baseSize);
    return data;
}

std::vector<uint8_t> GreedyRaftApplication::serializeAppendEntriesResponse(msg_appendentries_response_t* msg)
{
    std::vector<uint8_t> data(sizeof(*msg));
    memcpy(data.data(), msg, sizeof(*msg));
    return data;
}

void GreedyRaftApplication::deserializeRequestVote(const std::vector<uint8_t>& data, msg_requestvote_t* msg)
{
    if (data.size() >= sizeof(*msg)) {
        memcpy(msg, data.data(), sizeof(*msg));
    }
}

void GreedyRaftApplication::deserializeRequestVoteResponse(const std::vector<uint8_t>& data, msg_requestvote_response_t* msg)
{
    if (data.size() >= sizeof(*msg)) {
        memcpy(msg, data.data(), sizeof(*msg));
    }
}

void GreedyRaftApplication::deserializeAppendEntries(const std::vector<uint8_t>& data, msg_appendentries_t* msg)
{
    size_t baseSize = offsetof(msg_appendentries_t, entries);
    if (data.size() >= baseSize) {
        memcpy(msg, data.data(), baseSize);
        msg->entries = nullptr;
        msg->n_entries = 0;
    }
}

void GreedyRaftApplication::deserializeAppendEntriesResponse(const std::vector<uint8_t>& data, msg_appendentries_response_t* msg)
{
    if (data.size() >= sizeof(*msg)) {
        memcpy(msg, data.data(), sizeof(*msg));
    }
}

// ============ RAFT MESSAGE HANDLERS ============

void GreedyRaftApplication::handleRequestVote(const std::vector<uint8_t>& data, const std::string& packetName)
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
        responsePacketName << "greedy-raft-requestvote-response-from-" << myId_ 
                          << "-node-" << myRaftNodeId_
                          << "-to-" << senderVehicleId;
        
        auto payload = makeShared<BytesChunk>(responseData);
        auto packet = createPacket(responsePacketName.str());
        packet->insertAtBack(payload);
        sendPacket(std::move(packet));
        messagesSent_++;
    }
}

void GreedyRaftApplication::handleRequestVoteResponse(const std::vector<uint8_t>& data, const std::string& packetName)
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

void GreedyRaftApplication::handleAppendEntries(const std::vector<uint8_t>& data, const std::string& packetName)
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
        responsePacketName << "greedy-raft-appendentries-response-from-" << myId_ 
                          << "-node-" << myRaftNodeId_
                          << "-to-" << senderVehicleId;
        
        auto payload = makeShared<BytesChunk>(responseData);
        auto packet = createPacket(responsePacketName.str());
        packet->insertAtBack(payload);
        sendPacket(std::move(packet));
        messagesSent_++;
    }
}

void GreedyRaftApplication::handleAppendEntriesResponse(const std::vector<uint8_t>& data, const std::string& packetName)
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

// ============ GREEDY COORDINATION PROTOCOL ============

void GreedyRaftApplication::sendStatusRequest()
{
    std::ostringstream packetName;
    packetName << "greedy-coord-status-request-from-" << myId_ << "-broadcast";
    
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
    
    timerManager.create(
        veins::TimerSpecification([this]() {
            if (waitingForStatus_ && isLeader_ && !hasPassedIntersection_) {
                collectStatusAndDecideGreedy();
            }
        }).oneshotIn(SimTime(statusCollectionTimeoutMs_ / 1000.0))
    );
}

void GreedyRaftApplication::handleStatusRequest(int fromLeader)
{
    if (hasPassedIntersection_) return;
    sendStatusResponse(fromLeader, wayOfSight_);
}

void GreedyRaftApplication::sendStatusResponse(int toLeader, bool wos)
{
    std::ostringstream packetName;
    packetName << "greedy-coord-status-response-from-" << myId_ << "-to-" << toLeader;
    
    std::vector<uint8_t> data = {static_cast<uint8_t>(wos ? 1 : 0)};
    auto payload = makeShared<BytesChunk>(data);
    auto packet = createPacket(packetName.str());
    packet->insertAtBack(payload);
    sendPacket(std::move(packet));
    
    messagesSent_++;
}

void GreedyRaftApplication::handleStatusResponse(int fromVehicle, bool wos)
{
    if (!isLeader_ || !waitingForStatus_) return;
    
    collectedWayOfSight_[fromVehicle] = wos;
    statusResponseCount_++;
    
    // GREEDY: When leader has passed, only count active (non-passed) vehicles
    int expectedResponses = activeVehicles_.size();
    
    // If we got enough responses, decide
    if (statusResponseCount_ >= expectedResponses) {
        collectStatusAndDecideGreedy();
    }
}

void GreedyRaftApplication::collectStatusAndDecideGreedy()
{
    if (!isLeader_ || hasPassedIntersection_) return;
    
    waitingForStatus_ = false;
    
    // GREEDY SELECTION: Prioritize own lane first!
    int selectedVehicle = selectVehicleToPassGreedy();
    
    if (selectedVehicle >= 0) {
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

int GreedyRaftApplication::selectVehicleToPassGreedy()
{
    // GREEDY ALGORITHM:
    // 1. If there are lane-mates IN FRONT of me that can pass → command them first
    // 2. If no lane-mates in front, and I can pass → I pass
    // 3. Leader leaves after passing, new election happens
    //
    // This is "greedy" because the leader helps their own lane first
    
    // Get all active lane-mates and other lanes
    std::vector<int> myLaneMatesActive;
    std::vector<int> otherLanesCanPass;
    
    for (int vid : activeVehicles_) {
        if (isVehicleInMyLane(vid)) {
            myLaneMatesActive.push_back(vid);
        } else if (canVehiclePass(vid)) {
            otherLanesCanPass.push_back(vid);
        }
    }
    
    // Find lane-mates IN FRONT of me (lower position) that can pass
    std::vector<int> laneMatesInFrontCanPass;
    for (int vid : myLaneMatesActive) {
        if (vid == myId_) continue;
        int theirPos = getPositionInLane(vid);
        if (theirPos < myPositionInLane_ && canVehiclePass(vid)) {
            laneMatesInFrontCanPass.push_back(vid);
        }
    }
    
    // GREEDY PRIORITY 1: Coordinate lane-mates IN FRONT of me first
    if (!laneMatesInFrontCanPass.empty()) {
        // Select front-most (lowest position)
        int frontMost = laneMatesInFrontCanPass[0];
        for (int vid : laneMatesInFrontCanPass) {
            if (getPositionInLane(vid) < getPositionInLane(frontMost)) {
                frontMost = vid;
            }
        }
        std::cout << std::fixed << std::setprecision(1)
                  << (simTime().dbl() * 1000.0) << "ms [GREEDY] Leader " << myId_
                  << " (pos " << myPositionInLane_ << ") prioritizing LANE-MATE " 
                  << frontMost << " (pos " << getPositionInLane(frontMost) << ")" << std::endl;
        return frontMost;
    }
    
    // No lane-mates in front can pass - check if I can pass
    if (canVehiclePass(myId_)) {
        std::cout << std::fixed << std::setprecision(1)
                  << (simTime().dbl() * 1000.0) << "ms [GREEDY] Leader " << myId_
                  << " (pos " << myPositionInLane_ << ") passing SELF" << std::endl;
        return myId_;
    }
    
    // I can't pass yet - shouldn't happen in greedy, but pick from other lanes
    if (!otherLanesCanPass.empty()) {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(0, otherLanesCanPass.size() - 1);
        int selected = otherLanesCanPass[dis(gen)];
        std::cout << std::fixed << std::setprecision(1)
                  << (simTime().dbl() * 1000.0) << "ms [GREEDY] Leader " << myId_
                  << " selecting OTHER LANE vehicle " << selected << std::endl;
        return selected;
    }
    
    return -1;
}

void GreedyRaftApplication::sendPassCommand(int toVehicle)
{
    std::cout << std::fixed << std::setprecision(1)
              << (simTime().dbl() * 1000.0) << "ms [GREEDY] Leader " << myId_
              << " COMMANDS Vehicle " << toVehicle << " to PASS" << std::endl;
    
    std::ostringstream packetName;
    packetName << "greedy-coord-pass-command-from-" << myId_ << "-to-" << toVehicle;
    
    std::vector<uint8_t> data = {static_cast<uint8_t>(toVehicle)};
    auto payload = makeShared<BytesChunk>(data);
    auto packet = createPacket(packetName.str());
    packet->insertAtBack(payload);
    sendPacket(std::move(packet));
    
    messagesSent_++;
    
    if (toVehicle == myId_) {
        resumeMovement();
    }
}

void GreedyRaftApplication::handlePassCommand(int fromLeader)
{
    if (hasPassedIntersection_) return;
    
    std::cout << std::fixed << std::setprecision(1)
              << (simTime().dbl() * 1000.0) << "ms Vehicle " << myId_
              << " RECEIVED pass command from Leader " << fromLeader << std::endl;
    
    resumeMovement();
}

void GreedyRaftApplication::sendVehiclePassed()
{
    std::ostringstream packetName;
    packetName << "greedy-coord-vehicle-passed-from-" << myId_ << "-broadcast";
    
    std::vector<uint8_t> data = {static_cast<uint8_t>(myId_)};
    auto payload = makeShared<BytesChunk>(data);
    auto packet = createPacket(packetName.str());
    packet->insertAtBack(payload);
    sendPacket(std::move(packet));
    
    messagesSent_++;
}

void GreedyRaftApplication::handleVehiclePassed(int vehicleId)
{
    activeVehicles_.erase(vehicleId);
    
    // Update wayOfSight - if the passed vehicle was blocking me, I now have clear view
    if (vehicleInFrontOfMe_ == vehicleId) {
        wayOfSight_ = true;
        vehicleInFrontOfMe_ = -1;
        
        // Check if there's another vehicle in front on my lane
        int laneStartId = mySideIndex_ * vehiclesPerSide_;
        for (int pos = myPositionInLane_ - 1; pos >= 0; pos--) {
            int otherVehicle = laneStartId + pos;
            if (activeVehicles_.count(otherVehicle) > 0) {
                vehicleInFrontOfMe_ = otherVehicle;
                wayOfSight_ = false;
                break;
            }
        }
    }
    
    // Continue coordination if I'm leader and haven't passed
    if (isLeader_ && !hasPassedIntersection_ && !waitingForStatus_) {
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

void GreedyRaftApplication::checkAndStopAtIntersection()
{
    if (hasStoppedAtIntersection_ || !traciVehicle_) return;
    
    try {
        if (isAtIntersection() && !hasStoppedAtIntersection_) {
            stopVehicle();
            hasStoppedAtIntersection_ = true;
            timeStopped_ = simTime();
            intersectionEdge_ = traciVehicle_->getRoadId();
            calculateWayOfSight();
        }
    } catch (...) {}
}

void GreedyRaftApplication::calculateWayOfSight()
{
    if (myPositionInLane_ == 0) {
        wayOfSight_ = true;
        vehicleInFrontOfMe_ = -1;
    } else {
        int laneStartId = mySideIndex_ * vehiclesPerSide_;
        vehicleInFrontOfMe_ = -1;
        wayOfSight_ = true;
        
        for (int pos = myPositionInLane_ - 1; pos >= 0; pos--) {
            int otherVehicle = laneStartId + pos;
            if (activeVehicles_.count(otherVehicle) > 0) {
                vehicleInFrontOfMe_ = otherVehicle;
                wayOfSight_ = false;
                break;
            }
        }
    }
}

void GreedyRaftApplication::updateWayOfSightAfterPass(int passedVehicleId)
{
    // Called when a vehicle passes to update blocked vehicles
}

bool GreedyRaftApplication::isAtIntersection() const
{
    if (!traciVehicle_) return false;
    try {
        std::string roadId = traciVehicle_->getRoadId();
        return INTERSECTION_EDGES.count(roadId) > 0;
    } catch (...) {
        return false;
    }
}

bool GreedyRaftApplication::hasPassedIntersectionEdge() const
{
    if (!traciVehicle_ || intersectionEdge_.empty()) return false;
    try {
        std::string currentRoad = traciVehicle_->getRoadId();
        return currentRoad != intersectionEdge_;
    } catch (...) {
        return false;
    }
}

void GreedyRaftApplication::onBecameLeader()
{
    timeElected_ = simTime();
    wasElectedLeader_ = true;
    failedElectionCount_ = 0;
    
    double electionTimeMs = (timeElected_ - timeStopped_).dbl() * 1000.0;
    std::cout << std::fixed << std::setprecision(1)
              << (simTime().dbl() * 1000.0) << "ms [GREEDY] Vehicle " << myId_
              << " ELECTED LEADER (election took " << electionTimeMs << "ms)" << std::endl;
    
    if (hasStoppedAtIntersection_ && !hasPassedIntersection_) {
        sendStatusRequest();
    }
}

void GreedyRaftApplication::onLostLeadership()
{
    waitingForStatus_ = false;
    collectedWayOfSight_.clear();
}

void GreedyRaftApplication::resumeMovement()
{
    if (!traciVehicle_ || hasPassedIntersection_) return;
    
    try {
        traciVehicle_->setSpeed(-1);
        timeStartedMoving_ = simTime();
        
        std::cout << std::fixed << std::setprecision(1)
                  << (simTime().dbl() * 1000.0) << "ms Vehicle " << myId_
                  << " MOVING" << std::endl;
        
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
                    activeVehicles_.erase(myId_);
                    outputMetricsJSON();
                    
                    // Free RAFT - leader leaves, new election will happen
                    if (raftServer_) {
                        raft_free(raftServer_);
                        raftServer_ = nullptr;
                    }
                }
            }).oneshotIn(SimTime(passConfirmationMs_ / 1000.0))
        );
    } catch (...) {}
}

void GreedyRaftApplication::stopVehicle()
{
    if (!traciVehicle_) return;
    try {
        traciVehicle_->setSpeed(0);
    } catch (...) {}
}

void GreedyRaftApplication::handleFallback()
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

void GreedyRaftApplication::outputMetricsJSON()
{
    if (!resultsFileOpened_) return;
    
    double stoppedTimeMs = (timeStopped_ - timeArrived_).dbl() * 1000.0;
    double electionTimeMs = wasElectedLeader_ ? (timeElected_ - timeStopped_).dbl() * 1000.0 : 0;
    double totalWaitTimeMs = (timeStartedMoving_ - timeStopped_).dbl() * 1000.0;
    double transitTimeMs = (timePassed_ - timeStartedMoving_).dbl() * 1000.0;
    double throughputVehPerSec = 1.0 / ((timePassed_ - timeStopped_).dbl());
    
    if (vehiclesCompleted_ > 0) {
        resultsFile_ << ",\n";
    }
    
    resultsFile_ << "  {\n"
                 << "    \"vehicle_id\": " << myId_ << ",\n"
                 << "    \"total_vehicles\": " << totalVehicles_ << ",\n"
                 << "    \"algorithm\": \"greedy_raft\",\n"
                 << "    \"coordination_method\": \"" << coordinationMethod_ << "\",\n"
                 << "    \"was_leader\": " << (wasElectedLeader_ ? "true" : "false") << ",\n"
                 << "    \"way_of_sight\": " << (wayOfSight_ ? "true" : "false") << ",\n"
                 << "    \"lane\": \"" << myLane_ << "\",\n"
                 << "    \"side_index\": " << mySideIndex_ << ",\n"
                 << "    \"position_in_lane\": " << myPositionInLane_ << ",\n"
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
    
    if (vehiclesCompleted_ >= totalVehiclesStatic_) {
        closeResultsFile();
    }
}

// ============ UTILITY ============

raft_node_id_t GreedyRaftApplication::getNodeIdFromVehicleId(int vehicleId) const
{
    return vehicleId + 1;
}

int GreedyRaftApplication::getVehicleIdFromNodeId(raft_node_id_t nodeId) const
{
    return nodeId - 1;
}

void GreedyRaftApplication::sendCustomMessage(const std::string& type, const std::string& data)
{
    std::ostringstream packetName;
    packetName << type << "-from-" << myId_ << "-broadcast";
    
    std::vector<uint8_t> bytes(data.begin(), data.end());
    auto payload = makeShared<BytesChunk>(bytes);
    auto packet = createPacket(packetName.str());
    packet->insertAtBack(payload);
    sendPacket(std::move(packet));
    
    messagesSent_++;
}
