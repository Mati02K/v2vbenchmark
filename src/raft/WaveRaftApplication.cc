// WaveRaftApplication.cc — WAVE/802.11p transport subclass of RaftAppBase
// ~350 lines: only initialize, finish, onWSM, handleSelfMsg, handlePositionUpdate,
//             sendRaftToPeer, sendRaftBroadcast, getDistanceToJunction,
//             scheduleOneshotMs, and WAVE WSM message dispatch.
//
// All cluster formation, RAFT callbacks, serialization, coordination,
// batch scheduling, intersection detection, and metrics live in RaftAppBase.cc

#include "raft/WaveRaftApplication.h"
#include "raft/RaftWaveMessage_m.h"
#include "veins/modules/mobility/traci/TraCIScenarioManager.h"
#include <iostream>
#include <iomanip>
#include <sstream>

extern "C" {
#include "../../lib/raft/raft.h"
}

using namespace veins;

Define_Module(WaveRaftApplication);

// ============ STATIC MEMBERS ============
bool WaveRaftApplication::isGlobalInitialized_ = false;

// ============ CONSTRUCTOR / DESTRUCTOR ============

WaveRaftApplication::WaveRaftApplication()
{
    transportName_ = "wave";
    memset(&committedSchedule_, 0, sizeof(committedSchedule_));
}

WaveRaftApplication::~WaveRaftApplication() {}

// ============ EDGE PARAMETER PARSING (reads OMNeT++ par()) ============

void WaveRaftApplication::parseEdgeParametersFromNed()
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

// ============ INITIALIZATION ============

void WaveRaftApplication::initialize(int stage)
{
    DemoBaseApplLayer::initialize(stage);

    if (stage == 0) {
        myId_         = getParentModule()->getIndex();
        myRaftNodeId_ = myId_ + 1;

        try {
            // Read NED parameters (scenario-specific only; timing constants are in RaftAppBase constructor)
            totalVehicles_            = par("totalVehicles").intValue();
            intersectionStopDistance_ = par("intersectionStopDistance").doubleValue();
            clusterTriggerDistance_   = par("clusterTriggerDistance").doubleValue();
            discoveryBeaconInterval_  = par("discoveryBeaconInterval").doubleValue();
            resultsFileName_          = par("resultsFile").stdstringValue();
            resultsFileCloseAtSec_    = par("resultsFileCloseAtSec").doubleValue();
            isPriorityVehicle_        = par("isPriorityVehicle").boolValue();
            clusterMode_              = par("clusterMode").stdstringValue();
            allowMultipleRounds_      = par("allowMultipleRounds").boolValue();
        } catch (std::exception& e) {
            std::cerr << "Vehicle " << myId_ << " ERROR reading params: " << e.what() << std::endl;
        }

        // ---- Crypto init: generate keypair and get cert signed by appropriate CA ----
        memset(myPubKey_, 0, sizeof(myPubKey_));
        memset(&myCert_,  0, sizeof(myCert_));
        myPrivKey_ = CryptoAuth::instance().generateKeyPair(myPubKey_);
        std::string role   = isPriorityVehicle_ ? "priority" : "normal";
        std::string issuer = isPriorityVehicle_ ? "Emergency_CA" : "Vehicle_CA";
        myCert_ = CryptoAuth::instance().issueCert(myPubKey_, role, issuer);
        std::cout << "[V" << myId_ << "] Crypto init: role=" << role << " issuer=" << issuer << std::endl;

        parseEdgeParametersFromNed();

        // TODO : I need to understand why ghost vehicles spawns and clearly remove it
        // Ghost vehicle: SUMO recycled a vehicle slot after the original N vehicles
        // completed their routes.  myId_ >= totalVehicles_ means this module should
        // not participate in RAFT cluster formation or channel communication.
        // It may still drive through the intersection naturally via SUMO; we just
        // don't start any timers so it stays silent on the RAFT protocol layer.
        if (myId_ >= totalVehicles_) {
            std::cout << simTime() << " [DBG] Ghost vehicle V" << myId_
                      << " (totalVehicles=" << totalVehicles_ << ") — skipping RAFT init"
                      << std::endl;
            return;  // do not schedule checkTimer, discoveryTimer, or raftPeriodicTimer
        }

        int vehiclesPerSide = std::max(totalVehicles_ / 4, 1);
        int sideIndex       = myId_ / vehiclesPerSide;
        int posInLane       = myId_ % vehiclesPerSide;
        myLaneIndex_        = sideIndex % 4;

        if (!approachEdgeList_.empty()) {
            int dirIndex = sideIndex % (int)approachEdgeList_.size();
            myLane_ = approachEdgeList_[dirIndex];
        }
        static const char* routeNames[] = {"rN","rS","rE","rW"};  // North,South,East,West (N/W swapped)
        myRoute_ = routeNames[sideIndex % 4];

        initMyProposal();  // set static fields of cached proposal once

        vehicleInFrontOfMe_ = (posInLane > 0) ? myId_ - 1 : -1;  // initial guess
        wayOfSight_         = (posInLane == 0);
        isLaneLeader_       = (posInLane == 0);  // initial guess, updated dynamically

        activeVehicles_.insert(myId_);  // only self until RAFT forms via leader DB exchange

        if (!isGlobalInitialized_) {
            isGlobalInitialized_ = true;
            RaftMetrics::setTotalVehicles(totalVehicles_);
            RaftMetrics::openResultsFile(resultsFileName_);
            if (resultsFileCloseAtSec_ > 0) {
                closeResultsFileTimer_ = new cMessage("closeResultsFile");
                scheduleAt(resultsFileCloseAtSec_, closeResultsFileTimer_);
            }
        }

        timeArrived_ = simTime();
        clusterPhase_ = PHASE_DISCOVERY;

        // Create timers
        checkTimer_        = new cMessage("checkTimer");
        raftPeriodicTimer_ = new cMessage("raftPeriodicTimer");
        discoveryTimer_    = new cMessage("discoveryTimer");

        scheduleAt(simTime() + CHECK_INTERVAL, checkTimer_);
        scheduleAt(simTime() + uniform(0, discoveryBeaconInterval_), discoveryTimer_);

        // RAFT starts later via leader DB exchange at intersection (no initRaftSingleNode)
    }
}

void WaveRaftApplication::finish()
{
    if (!metricsWritten_ && RaftMetrics::isOpen() && hasStoppedAtIntersection_) {
        if (timeStartedMoving_ == SIMTIME_ZERO) timeStartedMoving_ = simTime();
        if (timePassed_ == SIMTIME_ZERO)        timePassed_        = simTime();
        outputMetricsJSON();
    }

    cancelAndDelete(checkTimer_);
    cancelAndDelete(raftPeriodicTimer_);
    if (closeResultsFileTimer_) { cancelEvent(closeResultsFileTimer_); delete closeResultsFileTimer_; closeResultsFileTimer_ = nullptr; }
    if (discoveryTimer_)     cancelAndDelete(discoveryTimer_);
    if (statusTimeoutTimer_) { cancelEvent(statusTimeoutTimer_); delete statusTimeoutTimer_; }
    if (passOrderTimer_)     { cancelEvent(passOrderTimer_);     delete passOrderTimer_; }
    if (fallbackTimer_)      { cancelEvent(fallbackTimer_);      delete fallbackTimer_; }
    if (arrivalWaitTimer_)   { cancelEvent(arrivalWaitTimer_);   delete arrivalWaitTimer_; }

    // Clean up oneshot timers
    for (cMessage* m : oneshotTimers_) {
        if (m->isScheduled()) cancelEvent(m);
        delete m;
    }
    oneshotTimers_.clear();

    if (raftServer_) { raft_free(raftServer_); raftServer_ = nullptr; }
    DemoBaseApplLayer::finish();
}

// ============ POSITION UPDATE ============
// runs every 100ms when vehicles moves.
void WaveRaftApplication::handlePositionUpdate(cObject* obj)
{
    DemoBaseApplLayer::handlePositionUpdate(obj);
    // changes needed for first time alone
    if (!mobility_) {
        mobility_ = TraCIMobilityAccess().get(getParentModule());
        if (mobility_) {
            traci_        = mobility_->getCommandInterface();
            traciVehicle_ = mobility_->getVehicleCommandInterface();
            // Colour priority vehicle red so it's visually distinct in SUMO-GUI
            if (isPriorityVehicle_) {
                try {
                    traciVehicle_->setColor(veins::TraCIColor(255, 0, 0, 255)); // red
                    std::cout << "[V" << myId_ << "] Priority vehicle coloured RED in SUMO-GUI" << std::endl;
                } catch (...) {}
            }
            try {
                std::string sumoId = mobility_->getExternalId();
                std::cout << simTime() << " [DBG] OMNeT++_V" << myId_
                          << " <=> SUMO_" << sumoId
                          << " laneIdx=" << myLaneIndex_
                          << " route=" << myRoute_ << std::endl;
            } catch (...) {}
        }
    }
    // Update lane leader flag every position update (~100ms TraCI step)
    if (traciVehicle_ && !hasPassedIntersection_)
        updateLaneLeaderFlag();
}

// ============ SELF MESSAGE HANDLING ============

void WaveRaftApplication::handleSelfMsg(cMessage* msg)
{
    if (msg == checkTimer_) {
        if (!hasPassedIntersection_) {
            updateLaneLeaderFlag();
        }
        
        if (!hasPassedIntersection_ && timeStartedMoving_ == SIMTIME_ZERO) {
            checkAndStopAtIntersection();
        }

        // if there is a front vehicle who moved at the intersection
        if (hasStoppedAtIntersection_ && !hasPassedIntersection_ && timeStartedMoving_ == SIMTIME_ZERO) 
        {
            checkAndAdvanceInQueue();
        }

        // vehicle left
        if ((hasStoppedAtIntersection_ || timeStartedMoving_ > SIMTIME_ZERO) && !hasPassedIntersection_)
        {
            checkIfLeftIntersection();
        }

        // only reschedule if we haven't passed the intersection yet; once we've passed, we can stop all timers and ignore future messages
        // this saves msg sent in bandwidth
        if (!hasPassedIntersection_)
        {
            scheduleAt(simTime() + CHECK_INTERVAL, checkTimer_);
        }
    }
    else if (msg == discoveryTimer_) {
        if (!hasPassedIntersection_ && !raftStarted_) {
            sendPeerBeacon();
        }
        if (!hasPassedIntersection_) {
            scheduleAt(simTime() + uniform(0, discoveryBeaconInterval_), discoveryTimer_);
        }
    }
    else if (msg == raftPeriodicTimer_) {
        processRaftPeriodic();
        if (!hasPassedIntersection_ && !isFallbackMode_)
            scheduleAt(simTime() + RAFT_PERIODIC_INTERVAL, raftPeriodicTimer_);
    }
    else if (strcmp(msg->getName(), "oneshotTimer") == 0) {
        auto it = oneshotCallbacks_.find(msg);
        if (it != oneshotCallbacks_.end()) {
            auto fn = it->second;
            oneshotCallbacks_.erase(it);
            oneshotTimers_.erase(std::remove(oneshotTimers_.begin(), oneshotTimers_.end(), msg),
                                              oneshotTimers_.end());
            delete msg;
            if (fn) fn();
        } else {
            std::cout << "Vehicle " << myId_ << " oneshot NOT FOUND" << std::endl;
            delete msg;
        }
    }
    else {
        DemoBaseApplLayer::handleSelfMsg(msg);
    }
}

// ============ WSM HANDLING ============

void WaveRaftApplication::onWSM(BaseFrame1609_4* frame)
{
    if (hasPassedIntersection_) return;

    benchmark::RaftWaveMessage* wsm = dynamic_cast<benchmark::RaftWaveMessage*>(frame);
    if (!wsm) return;

    int msgType  = wsm->getMsgType();
    int senderId = wsm->getSenderId();
    int targetId = wsm->getTargetId();

    // Extract payload bytes
    std::vector<uint8_t> payload;
    unsigned int pLen = wsm->getPayloadLen();
    payload.resize(pLen);
    for (unsigned int i = 0; i < pLen; i++) payload[i] = wsm->getPayload(i);

    // Not our unicast — drop (no relay)
    if (targetId != -1 && targetId != myId_) return;

    messagesReceived_++;

    int protocolSender = senderId;

    switch (msgType) {
        case benchmark::PEER_BEACON:
            handlePeerBeacon(payload, protocolSender);
            break;
        case benchmark::CLUSTER_JOIN_INVITE:
            handleClusterJoinInvite(payload, protocolSender);
            break;
        case benchmark::CLUSTER_FORM_BROADCAST:
            handleClusterFormBroadcast(payload);
            break;
        case benchmark::RAFT_REQUEST_VOTE:
            if (raftServer_) handleRequestVote(payload, protocolSender);
            break;
        case benchmark::RAFT_REQUEST_VOTE_RESPONSE:
            if (raftServer_) handleRequestVoteResponse(payload, protocolSender);
            break;
        case benchmark::RAFT_APPEND_ENTRIES:
            if (raftServer_) handleAppendEntries(payload, protocolSender);
            break;
        case benchmark::RAFT_APPEND_ENTRIES_RESPONSE:
            if (raftServer_) handleAppendEntriesResponse(payload, protocolSender);
            break;
        case benchmark::COORD_STATUS_REQUEST:
            handleStatusRequest(protocolSender);
            break;
        case benchmark::COORD_STATUS_RESPONSE:
            handleDbResponse(payload, protocolSender);
            break;
        case benchmark::COORD_VEHICLE_LEFT:
            if (payload.size() >= sizeof(VehicleLeftEntry)) {
                VehicleLeftEntry e;
                memcpy(&e, payload.data(), sizeof(e));
                handleVehicleLeft(e.vehicleId, e.batchId);
            }
            break;
        case benchmark::COORD_PASS_ORDER_BROADCAST:
            handlePassOrderBroadcast(payload);
            break;

        // ---- Quorum Certificate ----
        case benchmark::QC_SIGN_REQUEST:
            handleQCSignRequest(payload);
            break;
        case benchmark::QC_SIGN_RESPONSE:
            handleQCSignResponse(payload, protocolSender);
            break;
    }
}

// ============ WAVE RAFT MESSAGE SEND HELPERS ============

void WaveRaftApplication::sendRaftMessage(int msgType, int targetId,
                                                  const std::vector<uint8_t>& data)
{
    benchmark::RaftWaveMessage* wsm = new benchmark::RaftWaveMessage();
    populateWSM(wsm);
    wsm->setMsgType(msgType);
    wsm->setSenderId(myId_);
    wsm->setTargetId(targetId);
    wsm->setPayloadLen(data.size());
    wsm->setPayloadArraySize(data.size());
    for (size_t i = 0; i < data.size(); i++) wsm->setPayload(i, data[i]);
    wsm->setRecipientAddress(-1);
    sendDelayedDown(wsm, uniform(0.001, 0.005));
    messagesSent_++;
}

void WaveRaftApplication::broadcastRaftMessage(int msgType, const std::vector<uint8_t>& data)
{
    sendRaftMessage(msgType, -1, data);
}

// ============ TRANSPORT IMPLEMENTATION (RaftAppBase pure virtuals) ============

void WaveRaftApplication::sendRaftToPeer(int targetVehicleId, int msgType,
                                                  const std::vector<uint8_t>& data)
{
    sendRaftMessage(msgType, targetVehicleId, data);
}

void WaveRaftApplication::sendRaftBroadcast(int msgType, const std::vector<uint8_t>& data)
{
    sendRaftMessage(msgType, -1, data);
}

double WaveRaftApplication::getDistanceToJunction() const
{
    if (!traciVehicle_) return 999999.0;
    try {
        double lanePos  = traciVehicle_->getLanePosition();
        std::string lid = traciVehicle_->getLaneId();
        auto* manager   = veins::TraCIScenarioManagerAccess().get();
        if (!manager) return 999999.0;
        double laneLen  = manager->getCommandInterface()->lane(lid).getLength();
        return laneLen - lanePos;
    } catch (...) { return 999999.0; }
}

void WaveRaftApplication::onClusterFormed()
{
    // WAVE requires explicit starting of the periodic RAFT processing
    if (raftPeriodicTimer_ && !raftPeriodicTimer_->isScheduled()) {
        scheduleAt(simTime() + RAFT_PERIODIC_INTERVAL, raftPeriodicTimer_);
    }
}

double WaveRaftApplication::getRandomDouble(double lo, double hi)
{
    return uniform(lo, hi);
}

void WaveRaftApplication::scheduleOneshotMs(double delayMs, std::function<void()> fn)
{
    cMessage* tmsg = new cMessage("oneshotTimer");
    oneshotCallbacks_[tmsg] = fn;
    oneshotTimers_.push_back(tmsg);
    scheduleAt(simTime() + SimTime(delayMs / 1000.0), tmsg);
}

// ============ WAVE-SPECIFIC RAFT MESSAGE HANDLERS ============

void WaveRaftApplication::handleRequestVote(const std::vector<uint8_t>& data, int senderId)
{
    msg_requestvote_t msg;
    deserializeRequestVote(data, &msg);
    msg_requestvote_response_t resp;
    raft_recv_requestvote(raftServer_, raft_get_node(raftServer_, senderId+1), &msg, &resp);
    auto respData = serializeRequestVoteResponse(&resp);
    sendRaftMessage(benchmark::RAFT_REQUEST_VOTE_RESPONSE, senderId, respData);
}

void WaveRaftApplication::handleRequestVoteResponse(const std::vector<uint8_t>& data, int senderId)
{
    msg_requestvote_response_t msg;
    deserializeRequestVoteResponse(data, &msg);
    raft_recv_requestvote_response(raftServer_, raft_get_node(raftServer_, senderId+1), &msg);
}

void WaveRaftApplication::handleAppendEntries(const std::vector<uint8_t>& data, int senderId)
{
    msg_appendentries_t msg;
    deserializeAppendEntries(data, &msg);

    std::cout << std::fixed << std::setprecision(1) << (simTime().dbl()*1000.0) << "ms Vehicle " << myId_ << " RECEIVED AppendEntries from " << senderId
              << " (n_entries=" << msg.n_entries << ", leader_commit=" << msg.leader_commit << ")" << std::endl;

    msg_appendentries_response_t resp;
    raft_recv_appendentries(raftServer_, raft_get_node(raftServer_, senderId+1), &msg, &resp);

    if (msg.entries) delete[] msg.entries;

    auto respData = serializeAppendEntriesResponse(&resp);
    sendRaftMessage(benchmark::RAFT_APPEND_ENTRIES_RESPONSE, senderId, respData);
}

void WaveRaftApplication::handleAppendEntriesResponse(const std::vector<uint8_t>& data, int senderId)
{
    msg_appendentries_response_t msg;
    deserializeAppendEntriesResponse(data, &msg);
    raft_recv_appendentries_response(raftServer_, raft_get_node(raftServer_, senderId+1), &msg);
}
