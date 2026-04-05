// UdpRaftApplication.cc — UDP/INET transport subclass of RaftAppBase
// ~400 lines: only startApplication, stopApplication, processPacket,
//             sendRaftToPeer, sendRaftBroadcast, getDistanceToJunction,
//             scheduleOneshotMs, and UDP-specific RAFT message handlers.
//
// All cluster formation, RAFT callbacks, serialization, coordination,
// batch scheduling, intersection detection, and metrics live in RaftAppBase.cc

#include "raft/UdpRaftApplication.h"
#include "raft/RaftTypes_m.h"
#include "veins/modules/mobility/traci/TraCIColor.h"
#include <iostream>
#include <sstream>
#include <iomanip>
#include "inet/common/packet/Packet.h"
#include "inet/common/packet/chunk/BytesChunk.h"

extern "C" {
#include "../../lib/raft/raft.h"
}

using namespace inet;

Define_Module(UdpRaftApplication);

// ============ CONSTRUCTOR / DESTRUCTOR ============

UdpRaftApplication::UdpRaftApplication()
{
    transportName_ = "udp";
    memset(&committedPassOrder_, 0, sizeof(committedPassOrder_));
    memset(&committedSchedule_,  0, sizeof(committedSchedule_));
}

UdpRaftApplication::~UdpRaftApplication() {}

// ============ EDGE PARAMETER PARSING (reads OMNeT++ par()) ============

void UdpRaftApplication::parseEdgeParametersFromNed()
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

bool UdpRaftApplication::startApplication()
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
    invitationIntervalStoppedMs_ = par("invitationIntervalStoppedMs").intValue();
    resultsFileName_          = par("resultsFile").stdstringValue();
    resultsFileCloseAtSec_     = par("resultsFileCloseAtSec").doubleValue();

    statusCollectionTimeoutMs_ = par("statusCollectionTimeoutMs").intValue();
    fallbackClusterTimeoutMs_  = par("fallbackClusterTimeoutMs").intValue();
    discoveryWaitMs_          = par("discoveryWaitMs").intValue();
    clusterFormationDelayMs_  = par("clusterFormationDelayMs").intValue();
    mergeCooldownMs_         = par("mergeCooldownMs").intValue();
    vehicleLeftTimeoutMs_    = par("vehicleLeftTimeoutMs").intValue();
    isPriorityVehicle_             = par("isPriority vehicle").boolValue();

    // ---- Crypto init: generate keypair and get cert signed by appropriate CA ----
    memset(myPubKey_, 0, sizeof(myPubKey_));
    memset(&myCert_,  0, sizeof(myCert_));
    myPrivKey_ = CryptoAuth::instance().generateKeyPair(myPubKey_);
    std::string role   = isPriorityVehicle_ ? "priority" : "normal";
    std::string issuer = isPriorityVehicle_ ? "Emergency_CA" : "Vehicle_CA";
    myCert_ = CryptoAuth::instance().issueCert(myPubKey_, role, issuer);
    std::cout << "[V" << myId_ << "] Crypto init: role=" << role << " issuer=" << issuer << std::endl;

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

    initMyProposal();  // set static fields of cached proposal once

    vehicleInFrontOfMe_ = (posInLane > 0) ? myId_ - 1 : -1;  // initial guess, updated by updateLaneLeaderFlag
    wayOfSight_         = (posInLane == 0);
    isLaneLeader_       = (posInLane == 0);  // initial guess, updated dynamically

    activeVehicles_.insert(myId_);  // only self until RAFT forms via leader DB exchange

    RaftMetrics::setTotalVehicles(totalVehicles_);
    if (myId_ == 0) {
        RaftMetrics::openResultsFile(resultsFileName_);
        if (resultsFileCloseAtSec_ > 0) {
            closeResultsFileTimer_ = new cMessage("closeResultsFile");
            scheduleAt(resultsFileCloseAtSec_, closeResultsFileTimer_);
        }
    }

    // Get mobility / TraCI
    mobility_     = check_and_cast<veins::VeinsInetMobility*>(
                        getParentModule()->getSubmodule("mobility"));
    if (!mobility_) { EV_ERROR << "Missing mobility" << endl; return false; }
    traci_        = mobility_->getCommandInterface();
    traciVehicle_ = mobility_->getVehicleCommandInterface();
    if (!traci_ || !traciVehicle_) { EV_ERROR << "No TraCI" << endl; return false; }

    // Colour priority vehicle red so it's visually distinct in SUMO-GUI
    if (isPriorityVehicle_) {
        traciVehicle_->setColor(veins::TraCIColor(255, 0, 0, 255)); // red
        std::cout << "[V" << myId_ << "] Priority vehicle coloured RED in SUMO-GUI" << std::endl;
    }

    timeArrived_ = simTime();
    clusterPhase_ = PHASE_DISCOVERY;

    // Periodic check: intersection stop + left detection
    checkTimer_ = new cMessage("checkTimer");
    scheduleAt(simTime() + CHECK_INTERVAL, checkTimer_);

    // Discovery beacon timer
    discoveryTimer_ = new cMessage("discoveryTimer");
    scheduleAt(simTime() + uniform(0, discoveryBeaconInterval_), discoveryTimer_);

    // RAFT periodic timer (started by onClusterFormed when RAFT actually forms)
    raftPeriodicTimer_ = new cMessage("raftPeriodic");

    return true;
}

bool UdpRaftApplication::stopApplication()
{
    return true;
}

void UdpRaftApplication::finish()
{
    if (!metricsWritten_ && RaftMetrics::isOpen() && hasStoppedAtIntersection_) {
        if (timeStartedMoving_ == SIMTIME_ZERO) timeStartedMoving_ = simTime();
        if (timePassed_ == SIMTIME_ZERO)        timePassed_        = simTime();
        outputMetricsJSON();
    }
    
    // Clean up timers
    if (checkTimer_)         { cancelEvent(checkTimer_);         delete checkTimer_;         checkTimer_ = nullptr; }
    if (closeResultsFileTimer_) { cancelEvent(closeResultsFileTimer_); delete closeResultsFileTimer_; closeResultsFileTimer_ = nullptr; }
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

void UdpRaftApplication::handleMessageWhenUp(cMessage* msg)
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
        if (!hasPassedIntersection_ && !raftStarted_) {
            sendPeerBeacon();
        }
        if (!hasPassedIntersection_) {
            double nextInterval = uniform(0, discoveryBeaconInterval_);
            scheduleAt(simTime() + nextInterval, discoveryTimer_);
        }
    }
    else if (msg == raftPeriodicTimer_) {
        if (raftServer_ && !hasPassedIntersection_ && !isFallbackMode_)
            processRaftPeriodic();
        scheduleAt(simTime() + RAFT_PERIODIC_INTERVAL, raftPeriodicTimer_);
    }
    else if (msg == closeResultsFileTimer_) {
        RaftMetrics::closeResultsFile();
        cancelAndDelete(closeResultsFileTimer_);
        closeResultsFileTimer_ = nullptr;
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

void UdpRaftApplication::processPacket(std::shared_ptr<Packet> pk)
{
    if (hasPassedIntersection_) return;

    std::string pktName  = pk->getName();
    int  targetId        = extractTargetFromPacketName(pktName);
    bool isBroadcast     = (pktName.find("-broadcast") != std::string::npos);

    if (!isBroadcast && targetId != -1 && targetId != myId_) return;

    auto payload = pk->peekAtFront<BytesChunk>();
    if (!payload) return;
    const auto& bytes = payload->getBytes();
    if (bytes.size() < 1 || bytes.size() > 10000) return;

    messagesReceived_++;

    // First byte is the message type (set by sendRaftToPeer / sendRaftBroadcast)
    uint8_t msgType = bytes[0];
    const std::vector<uint8_t> data(bytes.begin() + 1, bytes.end());
    int sender = extractSenderFromPacketName(pktName);

    switch (msgType) {
        // ---- Discovery ----
        case benchmark::PEER_BEACON:
            handlePeerBeacon(data, sender);
            break;
        case benchmark::CLUSTER_JOIN_INVITE:
            handleClusterJoinInvite(data, sender);
            break;

        // ---- RAFT Core ----
        case benchmark::RAFT_REQUEST_VOTE:
            if (raftServer_) handleRequestVote(data, pktName);
            break;
        case benchmark::RAFT_REQUEST_VOTE_RESPONSE:
            if (raftServer_) handleRequestVoteResponse(data, pktName);
            break;
        case benchmark::RAFT_APPEND_ENTRIES:
            if (raftServer_) handleAppendEntries(data, pktName);
            break;
        case benchmark::RAFT_APPEND_ENTRIES_RESPONSE:
            if (raftServer_) handleAppendEntriesResponse(data, pktName);
            break;

        // ---- Coordination ----
        case benchmark::COORD_STATUS_REQUEST:
            handleStatusRequest(sender);
            break;
        case benchmark::COORD_STATUS_RESPONSE:
            handleDbResponse(data, sender);
            break;
        case benchmark::COORD_VEHICLE_PASSED:
            if (!data.empty()) handleVehiclePassed((int)data[0]);
            break;
        case benchmark::COORD_VEHICLE_LEFT:
            if (data.size() >= sizeof(VehicleLeftEntry)) {
                VehicleLeftEntry e;
                memcpy(&e, data.data(), sizeof(e));
                handleVehicleLeft(e.vehicleId, e.batchId);
            }
            break;
        case benchmark::COORD_PASS_ORDER_BROADCAST:
            handlePassOrderBroadcast(data);
            break;

        // ---- Quorum Certificate ----
        case benchmark::QC_SIGN_RESPONSE:
            handleQCSignResponse(data, sender);
            break;

        default:
            break;
    }
}

// ============ PACKET NAME HELPERS ============

int UdpRaftApplication::extractTargetFromPacketName(const std::string& n)
{
    size_t p = n.rfind("-to-");
    if (p == std::string::npos) return -1;
    try { return std::stoi(n.substr(p + 4)); } catch (...) { return -1; }
}

int UdpRaftApplication::extractSenderFromPacketName(const std::string& n)
{
    size_t fromPos = n.find("-from-");
    if (fromPos == std::string::npos) return -1;
    size_t endPos = n.find("-", fromPos + 6);
    if (endPos == std::string::npos) endPos = n.length();
    try { return std::stoi(n.substr(fromPos + 6, endPos - fromPos - 6)); } catch (...) { return -1; }
}

// ============ TRANSPORT IMPLEMENTATION ============

void UdpRaftApplication::sendRaftToPeer(int targetVehicleId, int msgType,
                                              const std::vector<uint8_t>& data)
{
    // Packet name encodes sender, optional raft node id (needed by response handlers), and target.
    std::ostringstream name;
    if (msgType == benchmark::RAFT_REQUEST_VOTE_RESPONSE ||
        msgType == benchmark::RAFT_APPEND_ENTRIES_RESPONSE)
        name << "raft-from-" << myId_ << "-node-" << myRaftNodeId_ << "-to-" << targetVehicleId;
    else
        name << "raft-from-" << myId_ << "-to-" << targetVehicleId;

    // Prepend msgType as first byte so receiver can switch on it
    std::vector<uint8_t> wire;
    wire.reserve(1 + data.size());
    wire.push_back(static_cast<uint8_t>(msgType));
    wire.insert(wire.end(), data.begin(), data.end());

    auto payload = makeShared<BytesChunk>(wire);
    auto pkt     = createPacket(name.str());
    pkt->insertAtBack(payload);
    sendPacket(std::move(pkt));
    messagesSent_++;
}

void UdpRaftApplication::sendRaftBroadcast(int msgType, const std::vector<uint8_t>& data)
{
    std::ostringstream name;
    name << "raft-from-" << myId_ << "-broadcast";

    // Prepend msgType as first byte so receiver can switch on it
    std::vector<uint8_t> wire;
    wire.reserve(1 + data.size());
    wire.push_back(static_cast<uint8_t>(msgType));
    wire.insert(wire.end(), data.begin(), data.end());

    auto payload = makeShared<BytesChunk>(wire);
    auto pkt     = createPacket(name.str());
    pkt->insertAtBack(payload);
    sendPacket(std::move(pkt));
    messagesSent_++;
}

double UdpRaftApplication::getDistanceToJunction() const
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

void UdpRaftApplication::scheduleOneshotMs(double delayMs, std::function<void()> fn)
{
    omnetpp::cMessage* msg = new omnetpp::cMessage("oneshotTimer");
    oneshotTimers_.push_back(msg);
    oneshotCallbacks_[msg] = fn;
    scheduleAt(simTime() + (delayMs / 1000.0), msg);
}

void UdpRaftApplication::onClusterFormed()
{
    if (raftPeriodicTimer_ && !raftPeriodicTimer_->isScheduled()) {
        scheduleAt(simTime() + RAFT_PERIODIC_INTERVAL, raftPeriodicTimer_);
    }
}

double UdpRaftApplication::getRandomDouble(double lo, double hi)
{
    return uniform(lo, hi);
}

// ============ UDP-SPECIFIC RAFT MESSAGE HANDLERS ============

void UdpRaftApplication::handleRequestVote(const std::vector<uint8_t>& data,
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
        sendRaftToPeer(senderVehicleId, 0x21, respData);
    }
}

void UdpRaftApplication::handleRequestVoteResponse(const std::vector<uint8_t>& data,
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

void UdpRaftApplication::handleAppendEntries(const std::vector<uint8_t>& data,
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
        sendRaftToPeer(senderVehicleId, 0x23, respData);
    }
    if (msg.entries) delete[] msg.entries;
}

void UdpRaftApplication::handleAppendEntriesResponse(const std::vector<uint8_t>& data,
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
