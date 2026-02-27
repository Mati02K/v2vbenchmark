// WillemtRaftApplication.cc — UDP/INET transport subclass of RaftAppBase
// ~400 lines: only startApplication, stopApplication, processPacket,
//             sendRaftUnicast, sendRaftBroadcast, getDistanceToJunction,
//             scheduleOneshotMs, and UDP-specific RAFT message handlers.
//
// All cluster formation, RAFT callbacks, serialization, coordination,
// batch scheduling, intersection detection, and metrics live in RaftAppBase.cc

#include "raft/WillemtRaftApplication.h"
#include <iostream>
#include <sstream>
#include <iomanip>
#include "inet/common/packet/Packet.h"
#include "inet/common/packet/chunk/BytesChunk.h"

extern "C" {
#include "../../third_party/raft/raft.h"
}

using namespace inet;

Define_Module(WillemtRaftApplication);

// ============ CONSTRUCTOR / DESTRUCTOR ============

WillemtRaftApplication::WillemtRaftApplication()
{
    transportName_ = "udp";
    memset(&committedPassOrder_, 0, sizeof(committedPassOrder_));
    memset(&committedSchedule_,  0, sizeof(committedSchedule_));
}

WillemtRaftApplication::~WillemtRaftApplication() {}

// ============ EDGE PARAMETER PARSING (reads OMNeT++ par()) ============

void WillemtRaftApplication::parseEdgeParametersFromNed()
{
    auto splitTrim = [](const std::string& s, char delim,
                        std::vector<std::string>& out,
                        std::set<std::string>& outSet) {
        std::stringstream ss(s);
        std::string tok;
        while (std::getline(ss, tok, delim)) {
            size_t a = tok.find_first_not_of(" \t");
            size_t b = tok.find_last_not_of(" \t");
            if (a != std::string::npos) {
                tok = tok.substr(a, b - a + 1);
                out.push_back(tok);
                outSet.insert(tok);
            }
        }
    };

    approachEdgeList_.clear(); exitEdgeList_.clear();
    intersectionEdges_.clear(); exitEdges_.clear();

    splitTrim(par("approachEdges").stdstringValue(), ',', approachEdgeList_, intersectionEdges_);
    splitTrim(par("exitEdges").stdstringValue(),     ',', exitEdgeList_,     exitEdges_);

    std::cout << "Vehicle " << myId_ << " edge config: "
              << approachEdgeList_.size() << " approach, "
              << exitEdgeList_.size() << " exit edges" << std::endl;
}

// ============ APPLICATION START ============

bool WillemtRaftApplication::startApplication()
{
    std::cout << "Vehicle RAFT (UDP) starting application." << std::endl;
    myId_         = getParentModule()->getIndex();
    myRaftNodeId_ = myId_ + 1;

    // Read NED parameters
    totalVehicles_            = par("totalVehicles").intValue();
    electionTimeoutBaseMs_    = par("electionTimeoutBaseMs").intValue();
    electionTimeoutJitterMs_  = par("electionTimeoutJitterMs").intValue();
    requestTimeoutMs_         = par("requestTimeoutMs").intValue();
    maxFailedElections_       = par("maxFailedElections").intValue();
    fallbackWaitMinMs_        = par("fallbackWaitMinMs").intValue();
    fallbackWaitMaxMs_        = par("fallbackWaitMaxMs").intValue();
    passConfirmationMs_       = par("passConfirmationMs").intValue();
    intersectionStopDistance_ = par("intersectionStopDistance").doubleValue();
    arrivalWaitTimeMs_        = par("arrivalWaitTimeMs").intValue();
    clusterTriggerDistance_   = par("clusterTriggerDistance").doubleValue();
    discoveryBeaconInterval_  = par("discoveryBeaconInterval").doubleValue();
    resultsFileName_          = par("resultsFile").stdstringValue();

    statusCollectionTimeoutMs_ = par("statusCollectionTimeoutMs").intValue();
    discoveryWaitMs_          = par("discoveryWaitMs").intValue();
    clusterFormationDelayMs_  = par("clusterFormationDelayMs").intValue();

    parseEdgeParametersFromNed();

    // Compute lane assignment
    int vehiclesPerSide = std::max(totalVehicles_ / 4, 1);
    int sideIndex       = myId_ / vehiclesPerSide;
    int posInLane       = myId_ % vehiclesPerSide;

    int dirIndex = sideIndex % (int)approachEdgeList_.size();
    myLane_      = approachEdgeList_[dirIndex];
    static const char* routeNames[] = {"rN","rS","rE","rW"};  // North,South,East,West (N/W swapped)
    myRoute_ = routeNames[dirIndex % 4];
    myLaneIndex_ = sideIndex % 4;

    vehicleInFrontOfMe_ = (posInLane > 0) ? myId_ - 1 : -1;
    wayOfSight_         = (posInLane == 0);

    for (int i = 0; i < totalVehicles_; i++) activeVehicles_.insert(i);

    RaftMetrics::setTotalVehicles(totalVehicles_);
    if (myId_ == 0) RaftMetrics::openResultsFile(resultsFileName_);

    // Get mobility / TraCI
    mobility_     = check_and_cast<veins::VeinsInetMobility*>(
                        getParentModule()->getSubmodule("mobility"));
    if (!mobility_) { EV_ERROR << "Missing mobility" << endl; return false; }
    traci_        = mobility_->getCommandInterface();
    traciVehicle_ = mobility_->getVehicleCommandInterface();
    if (!traci_ || !traciVehicle_) { EV_ERROR << "No TraCI" << endl; return false; }

    timeArrived_ = simTime();
    clusterPhase_ = PHASE_DISCOVERY;

    // Periodic check: intersection stop + left detection
    checkTimer_ = new cMessage("checkTimer");
    scheduleAt(simTime() + CHECK_INTERVAL, checkTimer_);

    // Discovery beacon timer
    discoveryTimer_ = new cMessage("discoveryTimer");
    scheduleAt(simTime() + uniform(0, discoveryBeaconInterval_), discoveryTimer_);

    // RAFT periodic timer
    raftPeriodicTimer_ = new cMessage("raftPeriodic");
    // Will be scheduled manually in onClusterFormed or checked periodic

    return true;
}

bool WillemtRaftApplication::stopApplication()
{
    return true;
}

void WillemtRaftApplication::finish()
{
    if (!metricsWritten_ && RaftMetrics::isOpen() && hasStoppedAtIntersection_) {
        if (timeStartedMoving_ == SIMTIME_ZERO) timeStartedMoving_ = simTime();
        if (timePassed_ == SIMTIME_ZERO)        timePassed_        = simTime();
        outputMetricsJSON();
    }
    
    // Clean up timers
    if (checkTimer_)         { cancelEvent(checkTimer_);         delete checkTimer_;         checkTimer_ = nullptr; }
    if (discoveryTimer_)     { cancelEvent(discoveryTimer_);     delete discoveryTimer_;     discoveryTimer_ = nullptr; }
    if (raftPeriodicTimer_)  { cancelEvent(raftPeriodicTimer_);  delete raftPeriodicTimer_;  raftPeriodicTimer_ = nullptr; }
    for (cMessage* m : oneshotTimers_) {
        if (m->isScheduled()) cancelEvent(m);
        delete m;
    }
    oneshotTimers_.clear();

    if (raftServer_) { raft_free(raftServer_); raftServer_ = nullptr; }
    VeinsInetApplicationBase::finish();
}

void WillemtRaftApplication::handleMessageWhenUp(cMessage* msg)
{
    if (msg == checkTimer_) {
        checkAndStopAtIntersection();
        checkAndAdvanceInQueue();
        if ((hasStoppedAtIntersection_ || timeStartedMoving_ > SIMTIME_ZERO) && !hasPassedIntersection_)
            checkIfLeftIntersection();
        if (!hasPassedIntersection_)
            scheduleAt(simTime() + CHECK_INTERVAL, checkTimer_);
    }
    else if (msg == discoveryTimer_) {
        if (clusterPhase_ == PHASE_DISCOVERY) {
            if (hasStoppedAtIntersection_) {
                sendDiscoveryBeacon();
                checkClusterTrigger();
            }
        } else if (clusterPhase_ == PHASE_COORDINATION && !hasPassedIntersection_) {
            sendDiscoveryBeacon();
            broadcastClusterExists();
        }
        // During RAFT coordination, fire beacons rarely so RAFT messages have the channel
        double nextBeacon = (clusterPhase_ == PHASE_COORDINATION)
                            ? 2.0
                            : uniform(0, discoveryBeaconInterval_);
        scheduleAt(simTime() + nextBeacon, discoveryTimer_);
    }
    else if (msg == raftPeriodicTimer_) {
        if (raftServer_ && !hasPassedIntersection_ && !isFallbackMode_)
            processRaftPeriodic();
        scheduleAt(simTime() + RAFT_PERIODIC_INTERVAL, raftPeriodicTimer_);
    }
    else if (msg->isSelfMessage() && strcmp(msg->getName(), "oneshotTimer") == 0) {
        auto it = oneshotCallbacks_.find(msg);
        if (it != oneshotCallbacks_.end()) {
            auto fn = it->second;
            oneshotCallbacks_.erase(it);
            oneshotTimers_.erase(std::remove(oneshotTimers_.begin(), oneshotTimers_.end(), msg),
                                              oneshotTimers_.end());
            delete msg;
            if (fn) fn();
        } else {
            delete msg;
        }
    }
    else if (msg->isSelfMessage()) {
        VeinsInetApplicationBase::handleMessageWhenUp(msg);
    }
    else {
        // Forward packets to socket
        socket.processMessage(msg);
    }
}

// ============ PACKET PROCESSING ============

void WillemtRaftApplication::processPacket(std::shared_ptr<Packet> pk)
{
    if (hasPassedIntersection_) return;

    std::string pktName  = pk->getName();
    int  targetId        = extractTargetFromPacketName(pktName);
    bool isBroadcast     = (pktName.find("-broadcast") != std::string::npos);

    if (!isBroadcast && targetId != -1 && targetId != myId_) return;

    auto payload = pk->peekAtFront<BytesChunk>();
    if (!payload) return;
    const auto& bytes = payload->getBytes();
    if (bytes.size() > 10000) return;

    messagesReceived_++;

    // RAFT protocol
    if      (pktName.find("raft-requestvote-response") != std::string::npos)
        handleRequestVoteResponse(bytes, pktName);
    else if (pktName.find("raft-requestvote") != std::string::npos)
        handleRequestVote(bytes, pktName);
    else if (pktName.find("raft-appendentries-response") != std::string::npos)
        handleAppendEntriesResponse(bytes, pktName);
    else if (pktName.find("raft-appendentries") != std::string::npos)
        handleAppendEntries(bytes, pktName);

    // Coordination
    else if (pktName.find("coord-status-request") != std::string::npos)
        handleStatusRequest(extractSenderFromPacketName(pktName));
    else if (pktName.find("coord-status-response") != std::string::npos) {
        int from = extractSenderFromPacketName(pktName);
        if (bytes.size() >= sizeof(VehicleProposal)) {
            VehicleProposal proposal;
            memcpy(&proposal, bytes.data(), sizeof(VehicleProposal));
            handleStatusResponseProposal(from, proposal);
        }
    }
    else if (pktName.find("coord-vehicle-passed") != std::string::npos) {
        if (bytes.size() >= 1) handleVehiclePassed((int)bytes[0]);
    }
    else if (pktName.find("coord-vehicle-left") != std::string::npos) {
        if (bytes.size() >= sizeof(VehicleLeftEntry) + 1) {
            VehicleLeftEntry e;
            memcpy(&e, bytes.data(), sizeof(e));
            int ttl = static_cast<int>(bytes[sizeof(VehicleLeftEntry)]);
            handleVehicleLeftGossip(e.vehicleId, e.batchId, ttl);
        } else if (bytes.size() >= sizeof(VehicleLeftEntry)) {
            VehicleLeftEntry e;
            memcpy(&e, bytes.data(), sizeof(e));
            handleVehicleLeft(e.vehicleId);
        } else if (bytes.size() >= 1) {
            handleVehicleLeft((int)bytes[0]);
        }
    }
    // Discovery
    else if (pktName.find("discovery-beacon") != std::string::npos) {
        int senderId = extractSenderFromPacketName(pktName);
        if (senderId != myId_ && bytes.size() >= sizeof(int) + 1) {
            uint8_t senderPhase = bytes[sizeof(int)];
            if (senderPhase == PHASE_DISCOVERY)
                handleDiscoveryBeacon(senderId, senderPhase);
            else
                discoveredPeers_.erase(senderId);
        }
    }
    else if (pktName.find("cluster-form") != std::string::npos)
        handleClusterForm(bytes, extractSenderFromPacketName(pktName));
    else if (pktName.find("cluster-exists") != std::string::npos)
        handleClusterExists(bytes, extractSenderFromPacketName(pktName));
}

// ============ PACKET NAME HELPERS ============

int WillemtRaftApplication::extractTargetFromPacketName(const std::string& n)
{
    size_t p = n.rfind("-to-");
    if (p == std::string::npos) return -1;
    try { return std::stoi(n.substr(p + 4)); } catch (...) { return -1; }
}

int WillemtRaftApplication::extractSenderFromPacketName(const std::string& n)
{
    size_t fromPos = n.find("-from-");
    if (fromPos == std::string::npos) return -1;
    size_t endPos = n.find("-", fromPos + 6);
    if (endPos == std::string::npos) endPos = n.length();
    try { return std::stoi(n.substr(fromPos + 6, endPos - fromPos - 6)); } catch (...) { return -1; }
}

// ============ TRANSPORT IMPLEMENTATION ============

void WillemtRaftApplication::sendRaftUnicast(int targetVehicleId, int msgType,
                                              const std::vector<uint8_t>& data)
{
    // Build packet name from msgType (we map numeric types back to names for clarity)
    std::ostringstream name;
    switch (msgType) {
        case 0x20: name << "raft-requestvote-from-"         << myId_ << "-to-" << targetVehicleId; break;
        case 0x21: name << "raft-requestvote-response-from-" << myId_ << "-node-" << myRaftNodeId_ << "-to-" << targetVehicleId; break;
        case 0x22: name << "raft-appendentries-from-"       << myId_ << "-to-" << targetVehicleId; break;
        case 0x23: name << "raft-appendentries-response-from-" << myId_ << "-node-" << myRaftNodeId_ << "-to-" << targetVehicleId; break;
        case 0x31: name << "coord-status-response-from-"    << myId_ << "-to-" << targetVehicleId; break;
        default:   name << "raft-msg-" << msgType << "-from-" << myId_ << "-to-" << targetVehicleId; break;
    }

    auto payload = makeShared<BytesChunk>(data);
    auto pkt     = createPacket(name.str());
    pkt->insertAtBack(payload);
    sendPacket(std::move(pkt));
    messagesSent_++;
}

void WillemtRaftApplication::sendRaftBroadcast(int msgType, const std::vector<uint8_t>& data)
{
    std::ostringstream name;
    switch (msgType) {
        case 0x10: name << "discovery-beacon-from-" << myId_; break;
        case 0x11: name << "cluster-form-from-"     << myId_ << "-broadcast"; break;
        case 0x30: name << "coord-status-request-from-" << myId_ << "-broadcast"; break;
        case 0x33: name << "coord-vehicle-passed-from-" << myId_ << "-broadcast"; break;
        case 0x34: name << "coord-vehicle-left-from-"   << myId_ << "-broadcast"; break;
        case 0x35: name << "coord-vehicle-left-rebroadcast-from-" << myId_ << "-broadcast"; break;
        default:   name << "raft-bcast-" << msgType << "-from-" << myId_ << "-broadcast"; break;
    }

    auto payload = makeShared<BytesChunk>(data);
    auto pkt     = createPacket(name.str());
    pkt->insertAtBack(payload);
    sendPacket(std::move(pkt));
    messagesSent_++;
}

double WillemtRaftApplication::getDistanceToJunction() const
{
    if (!traciVehicle_) return 999999.0;
    try {
        double lanePos  = traciVehicle_->getLanePosition();
        std::string lid = traciVehicle_->getLaneId();
        // Use TraCI to get lane length
        double laneLen  = traci_->lane(lid).getLength();
        return laneLen - lanePos;
    } catch (...) { return 999999.0; }
}

void WillemtRaftApplication::scheduleOneshotMs(double delayMs, std::function<void()> fn)
{
    omnetpp::cMessage* msg = new omnetpp::cMessage("oneshotTimer");
    oneshotTimers_.push_back(msg);
    oneshotCallbacks_[msg] = fn;
    scheduleAt(simTime() + (delayMs / 1000.0), msg);
}

void WillemtRaftApplication::onClusterFormed()
{
    if (raftPeriodicTimer_ && !raftPeriodicTimer_->isScheduled()) {
        scheduleAt(simTime() + RAFT_PERIODIC_INTERVAL, raftPeriodicTimer_);
    }
}

double WillemtRaftApplication::getRandomDouble(double lo, double hi)
{
    return uniform(lo, hi);
}

// ============ UDP-SPECIFIC RAFT MESSAGE HANDLERS ============

void WillemtRaftApplication::handleRequestVote(const std::vector<uint8_t>& data,
                                                const std::string& pktName)
{
    if (!raftServer_) return;
    msg_requestvote_t msg;
    deserializeRequestVote(data, &msg);
    raft_node_t* node = raft_get_node(raftServer_, msg.candidate_id);
    if (!node) return;

    msg_requestvote_response_t resp;
    if (raft_recv_requestvote(raftServer_, node, &msg, &resp) == 0) {
        int senderVehicleId = msg.candidate_id - 1;
        auto respData = serializeRequestVoteResponse(&resp);
        sendRaftUnicast(senderVehicleId, 0x21, respData);
    }
}

void WillemtRaftApplication::handleRequestVoteResponse(const std::vector<uint8_t>& data,
                                                        const std::string& pktName)
{
    if (!raftServer_) return;
    msg_requestvote_response_t msg;
    deserializeRequestVoteResponse(data, &msg);

    size_t nodePos = pktName.find("-node-");
    size_t toPos   = pktName.find("-to-", nodePos);
    if (nodePos == std::string::npos || toPos == std::string::npos) return;
    try {
        raft_node_id_t senderNodeId = std::stoi(pktName.substr(nodePos+6, toPos-nodePos-6));
        raft_node_t* node = raft_get_node(raftServer_, senderNodeId);
        if (node) raft_recv_requestvote_response(raftServer_, node, &msg);
    } catch (...) {}
}

void WillemtRaftApplication::handleAppendEntries(const std::vector<uint8_t>& data,
                                                  const std::string& pktName)
{
    if (!raftServer_) return;
    msg_appendentries_t msg;
    deserializeAppendEntries(data, &msg);

    int senderVehicleId = extractSenderFromPacketName(pktName);
    if (senderVehicleId < 0) return;

    std::cout << std::fixed << std::setprecision(1)
              << (simTime().dbl()*1000.0) << "ms Vehicle " << myId_
              << " RECEIVED AppendEntries from vehicle " << senderVehicleId
              << " (n_entries=" << msg.n_entries
              << ", leader_commit=" << msg.leader_commit << ")" << std::endl;

    raft_node_id_t senderNodeId = senderVehicleId + 1;
    raft_node_t* node = raft_get_node(raftServer_, senderNodeId);
    if (node) {
        msg_appendentries_response_t resp;
        raft_recv_appendentries(raftServer_, node, &msg, &resp);
        auto respData = serializeAppendEntriesResponse(&resp);
        sendRaftUnicast(senderVehicleId, 0x23, respData);
    }
    if (msg.entries) delete[] msg.entries;
}

void WillemtRaftApplication::handleAppendEntriesResponse(const std::vector<uint8_t>& data,
                                                          const std::string& pktName)
{
    if (!raftServer_) return;
    msg_appendentries_response_t msg;
    deserializeAppendEntriesResponse(data, &msg);

    size_t nodePos = pktName.find("-node-");
    size_t toPos   = pktName.find("-to-", nodePos);
    if (nodePos == std::string::npos || toPos == std::string::npos) return;
    try {
        raft_node_id_t senderNodeId = std::stoi(pktName.substr(nodePos+6, toPos-nodePos-6));
        raft_node_t* node = raft_get_node(raftServer_, senderNodeId);
        if (node) raft_recv_appendentries_response(raftServer_, node, &msg);
    } catch (...) {}
}
