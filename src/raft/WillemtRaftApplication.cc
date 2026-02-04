// WillemtRaftApplication.cc - COMPLETE IMPLEMENTATION WITH QUORUM CONSENSUS
// Part 1: Includes, statics, constructor, initialization

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

// ============ STATIC MEMBERS ============

const std::set<std::string> WillemtRaftApplication::INTERSECTION_EDGES = {
    "N2C", "S2C", "E2C", "W2C",
    "C2S", "C2N", "C2E", "C2W"
};

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

// ============ CONSTRUCTOR / DESTRUCTOR ============

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
    , lastAppliedIndex_(0)
    , waitingForVehicle_(-1)
    , hasCommittedOrder_(false)
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
    
    resultsFileName_ = par("resultsFile").stdstringValue();
    
    // Calculate vehicle's lane
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
    
    // Set timeouts
    // Task 1: Random Jitter to prevent split-vote deadlocks in UDP
    int electionTimeout = electionTimeoutBaseMs_ + intuniform(0, electionTimeoutJitterMs_);
    raft_set_election_timeout(raftServer_, electionTimeout);
    raft_set_request_timeout(raftServer_, requestTimeoutMs_);
    
    // Add all nodes to cluster
    for (int i = 0; i < totalVehicles_; i++) {
        int nodeId = i + 1;
        void* userData = reinterpret_cast<void*>(static_cast<intptr_t>(i));
        int isSelf = (i == myId_) ? 1 : 0;
        raft_add_node(raftServer_, userData, nodeId, isSelf);
    }
    
    timeArrived_ = simTime();
    
    // Schedule periodic checks
    timerManager.create(
        veins::TimerSpecification([this]() {
            checkAndStopAtIntersection();
        }).interval(SimTime(CHECK_INTERVAL))
    );
    
    timerManager.create(
        veins::TimerSpecification([this]() {
            processRaftPeriodic();
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
        if (hasStoppedAtIntersection_ && !hasCommittedOrder_ && !isFallbackMode_) {
            simtime_t waitTime = now - timeStopped_;
            double timeoutThreshold = (double)totalVehiclesStatic_;
            if (waitTime.dbl() > timeoutThreshold) {  // Dynamic timeout based on scale
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

        // Store committed pass order
        memcpy(&committedPassOrder_, passOrder, sizeof(PassOrderEntry));
        hasCommittedOrder_ = true;
        
        // Record when PASS_ORDER was committed (end of RAFT decision)
        timeOrderCommitted_ = simTime();

        logEntriesCommitted_++;
        lastAppliedIndex_ = entry_idx;

        // Execute pass order
        executePassOrder();

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

        // Clean up pending proposals
        pendingProposals_.erase(entry_idx);
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
    for (const auto& [vid, status] : committedStatuses_) {
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

    // Create deterministic order
    std::vector<int> order = createDeterministicOrder();

    std::cout << std::fixed << std::setprecision(1)
              << (simTime().dbl() * 1000.0) << "ms Leader " << myId_
              << " PROPOSING PASS_ORDER [";
    for (size_t i = 0; i < order.size(); i++) {
        std::cout << order[i];
        if (i < order.size() - 1) std::cout << ",";
    }
    std::cout << "] to quorum" << std::endl;

    // Create pass order entry
    PassOrderEntry passOrder;
    passOrder.numVehicles = std::min((int)order.size(), 32);
    for (int i = 0; i < passOrder.numVehicles; i++) {
        passOrder.order[i] = order[i];
    }

    // Prepare entry buffer: [type byte][pass order data]
    size_t entrySize = 1 + sizeof(PassOrderEntry);
    std::vector<uint8_t> entryBuffer(entrySize);
    entryBuffer[0] = static_cast<uint8_t>(PASS_ORDER);
    memcpy(entryBuffer.data() + 1, &passOrder, sizeof(PassOrderEntry));

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
                  << " submitted PASS_ORDER entry #" << response.idx << std::endl;
        
        // CRITICAL: Leader must apply its own committed entry
        // Instead of retrieving from RAFT log (which has corruption issues),
        // directly apply the pass order data we just created
        timerManager.create(
            veins::TimerSpecification([this, idx = response.idx, passOrder]() {
                if (raftServer_ && raft_get_commit_idx(raftServer_) >= idx) {
                    std::cout << std::fixed << std::setprecision(1)
                              << (simTime().dbl() * 1000.0) << "ms Leader " << myId_
                              << " APPLYING committed PASS_ORDER entry #" << idx << " [";
                    for (int i = 0; i < passOrder.numVehicles; i++) {
                        std::cout << passOrder.order[i];
                        if (i < passOrder.numVehicles - 1) std::cout << ",";
                    }
                    std::cout << "]" << std::endl;
                    
                    // Store committed pass order
                    memcpy(&committedPassOrder_, &passOrder, sizeof(PassOrderEntry));
                    hasCommittedOrder_ = true;
                    logEntriesCommitted_++;
                    lastAppliedIndex_ = idx;
                    
                    // Execute pass order
                    executePassOrder();
                }
            }).oneshotIn(SimTime(0.05))  // Check after 50ms
        );
    } else {
        std::cout << std::fixed << std::setprecision(1)
                  << (simTime().dbl() * 1000.0) << "ms Leader " << myId_
                  << " FAILED to propose PASS_ORDER (error " << result << ")" << std::endl;
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
            int vPS = totalVehicles_ / 4;
            if (vPS == 0) vPS = 1;
            int otherSide = otherVid / vPS;
            int mySide = myId_ / vPS;
            if ((mySide == 0 || mySide == 1) && (otherSide == 0 || otherSide == 1)) return false;
            if ((mySide == 2 || mySide == 3) && (otherSide == 2 || otherSide == 3)) return false;
            return true;
        };
        for (int i = 0; i < myPosition; i++) {
            if (hasConflict(committedPassOrder_.order[i])) totalDelayMs += 150;
        }
        int delayMs = totalDelayMs;
        
        std::cout << std::fixed << std::setprecision(1)
                  << (simTime().dbl() * 1000.0) << "ms Vehicle " << myId_
                  << " at position " << myPosition << ", waiting " << delayMs << "ms for turn" << std::endl;
        
        // Wait for calculated delay, then move
        timerManager.create(
            veins::TimerSpecification([this, delayMs]() {
                if (!hasPassedIntersection_) {
                    std::cout << std::fixed << std::setprecision(1)
                              << (simTime().dbl() * 1000.0) << "ms Vehicle " << myId_
                              << " starting movement (after " << delayMs << "ms delay)" << std::endl;
                    resumeMovement();
                }
            }).oneshotIn(SimTime(delayMs / 1000.0))
        );
    }
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
              << " BROADCASTING vehicle-left message" << std::endl;

    std::ostringstream packetName;
    packetName << "coord-vehicle-left-from-" << myId_ << "-broadcast";

    std::vector<uint8_t> data = {static_cast<uint8_t>(myId_)};
    auto payload = makeShared<BytesChunk>(data);
    auto packet = createPacket(packetName.str());
    packet->insertAtBack(payload);
    sendPacket(std::move(packet));

    messagesSent_++;
}

void WillemtRaftApplication::rebroadcastVehicleLeft(int vehicleId)
{
    std::ostringstream packetName;
    packetName << "coord-vehicle-left-rebroadcast-from-" << myId_ << "-for-" << vehicleId;

    std::vector<uint8_t> data = {static_cast<uint8_t>(vehicleId)};
    auto payload = makeShared<BytesChunk>(data);
    auto packet = createPacket(packetName.str());
    packet->insertAtBack(payload);
    sendPacket(std::move(packet));

    messagesSent_++;
}

void WillemtRaftApplication::handleVehicleLeft(int vehicleId)
{
    std::cout << std::fixed << std::setprecision(1)
              << (simTime().dbl() * 1000.0) << "ms Vehicle " << myId_
              << " RECEIVED vehicle-left from vehicle " << vehicleId << std::endl;

    // Remove from active set
    activeVehicles_.erase(vehicleId);
    
    // OPTIMIZATION: Check if it's my turn FIRST, then move immediately
    bool isMyTurn = (vehicleId == waitingForVehicle_);
    
    if (isMyTurn) {
        std::cout << std::fixed << std::setprecision(1)
                  << (simTime().dbl() * 1000.0) << "ms Vehicle " << myId_
                  << " previous vehicle left, MY TURN - starting immediately!" << std::endl;
        
        // Start moving immediately
        resumeMovement();
        waitingForVehicle_ = -1;
    }
    
    // NOTE: Removed rebroadcast - it was causing broadcast storm
    // The original broadcast from the vehicle is sufficient
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
        pendingProposals_[response.idx] = cmdData;

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
        if (isAtIntersection() && !hasStoppedAtIntersection_) {
            stopVehicle();
            hasStoppedAtIntersection_ = true;
            timeStopped_ = simTime();
            intersectionEdge_ = traciVehicle_->getRoadId();
            calculateWayOfSight();
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

    if (hasStoppedAtIntersection_ && !hasPassedIntersection_) {
        sendStatusRequest();
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
        // Task 3: God Mode (Safety Overrides)
        // Match WAVE implementation and disable SUMO's defensive driving
        traciVehicle_->setSpeedMode(0); // Ignore SUMO collisions/safety
        traciVehicle_->setSpeed(-1);   // Resume speed
        
        // Ignore priority junction rules
        traciVehicle_->setParameter("jmIgnoreFoeProb", "1.0");
        traciVehicle_->setParameter("jmIgnoreFoeSpeed", "100.0");
        traciVehicle_->setParameter("jmTimegapMinor", "0.0");
        
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
        
        // STRICT HONEST MODE: Explicitly check for Exit Edges only.
        // Logic copied from WAVE implementation.
        bool isExit = (currentRoad == "C2S" || currentRoad == "C2N" || 
                       currentRoad == "C2E" || currentRoad == "C2W");
        
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

    double stoppedTimeMs = (timeStopped_ - timeArrived_).dbl() * 1000.0;
    double electionTimeMs = wasElectedLeader_ ? (timeElected_ - timeStopped_).dbl() * 1000.0 : 0;
    double raftDecisionTimeMs = (timeOrderCommitted_ - timeStopped_).dbl() * 1000.0;  // RAFT consensus time
    double totalWaitTimeMs = (timeStartedMoving_ - timeStopped_).dbl() * 1000.0;
    double transitTimeMs = (timePassed_ - timeStartedMoving_).dbl() * 1000.0;
    double throughputVehPerSec = 1.0 / ((timePassed_ - timeStopped_).dbl());

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
                 << "    \"log_entries_proposed\": " << logEntriesProposed_ << ",\n"
                 << "    \"log_entries_committed\": " << logEntriesCommitted_ << ",\n"
                 << "    \"timestamps_ms\": {\n"
                 << "      \"arrived\": " << std::fixed << std::setprecision(2) << (timeArrived_.dbl() * 1000.0) << ",\n"
                 << "      \"stopped\": " << (timeStopped_.dbl() * 1000.0) << ",\n"
                 << "      \"elected\": " << (timeElected_.dbl() * 1000.0) << ",\n"
                 << "      \"order_committed\": " << (timeOrderCommitted_.dbl() * 1000.0) << ",\n"
                 << "      \"started_moving\": " << (timeStartedMoving_.dbl() * 1000.0) << ",\n"
                 << "      \"passed\": " << (timePassed_.dbl() * 1000.0) << "\n"
                 << "    },\n"
                 << "    \"durations_ms\": {\n"
                 << "      \"approach_time\": " << stoppedTimeMs << ",\n"
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
