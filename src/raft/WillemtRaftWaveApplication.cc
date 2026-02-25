// WillemtRaftWaveApplication.cc — WAVE/802.11p transport subclass of RaftAppBase
// ~350 lines: only initialize, finish, onWSM, handleSelfMsg, handlePositionUpdate,
//             sendRaftUnicast, sendRaftBroadcast, getDistanceToJunction,
//             scheduleOneshotMs, and WAVE WSM message dispatch.
//
// All cluster formation, RAFT callbacks, serialization, coordination,
// batch scheduling, intersection detection, and metrics live in RaftAppBase.cc

#include "raft/WillemtRaftWaveApplication.h"
#include "raft/RaftWaveMessage_m.h"
#include "veins/modules/mobility/traci/TraCIScenarioManager.h"
#include <iostream>
#include <iomanip>
#include <sstream>

extern "C" {
#include "../../third_party/raft/raft.h"
}

using namespace veins;

Define_Module(WillemtRaftWaveApplication);

// ============ STATIC MEMBERS ============
bool WillemtRaftWaveApplication::isGlobalInitialized_ = false;

// ============ CONSTRUCTOR / DESTRUCTOR ============

WillemtRaftWaveApplication::WillemtRaftWaveApplication()
{
    transportName_ = "wave";
    memset(&committedPassOrder_, 0, sizeof(committedPassOrder_));
    memset(&committedSchedule_,  0, sizeof(committedSchedule_));
}

WillemtRaftWaveApplication::~WillemtRaftWaveApplication() {}

// ============ EDGE PARAMETER PARSING (reads OMNeT++ par()) ============

void WillemtRaftWaveApplication::parseEdgeParametersFromNed()
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

void WillemtRaftWaveApplication::initialize(int stage)
{
    DemoBaseApplLayer::initialize(stage);

    if (stage == 0) {
        myId_         = getParentModule()->getIndex();
        myRaftNodeId_ = myId_ + 1;

        try {
            totalVehicles_            = par("totalVehicles").intValue();
            electionTimeoutBaseMs_    = par("electionTimeoutBaseMs").intValue();
            electionTimeoutJitterMs_  = par("electionTimeoutJitterMs").intValue();
            requestTimeoutMs_         = par("requestTimeoutMs").intValue();
            maxFailedElections_       = par("maxFailedElections").intValue();
            fallbackWaitMinMs_        = par("fallbackWaitMinMs").intValue();
            fallbackWaitMaxMs_        = par("fallbackWaitMaxMs").intValue();
            passConfirmationMs_       = par("passConfirmationMs").intValue();
            statusCollectionTimeoutMs_ = par("statusCollectionTimeoutMs").intValue();
            intersectionStopDistance_ = par("intersectionStopDistance").doubleValue();
            arrivalWaitTimeMs_        = par("arrivalWaitTimeMs").intValue();
            clusterTriggerDistance_   = par("clusterTriggerDistance").doubleValue();
            discoveryBeaconInterval_  = par("discoveryBeaconInterval").doubleValue();
            resultsFileName_          = par("resultsFile").stdstringValue();
        } catch (std::exception& e) {
            std::cerr << "Vehicle " << myId_ << " ERROR reading params: " << e.what() << std::endl;
        }

        parseEdgeParametersFromNed();

        int vehiclesPerSide = std::max(totalVehicles_ / 4, 1);
        int sideIndex       = myId_ / vehiclesPerSide;
        int posInLane       = myId_ % vehiclesPerSide;
        myLaneIndex_        = sideIndex % 4;

        if (!approachEdgeList_.empty()) {
            int dirIndex = sideIndex % (int)approachEdgeList_.size();
            myLane_ = approachEdgeList_[dirIndex];
        }
        static const char* routeNames[] = {"rW","rS","rE","rN"};
        myRoute_ = routeNames[sideIndex % 4];

        vehicleInFrontOfMe_ = (posInLane > 0) ? myId_ - 1 : -1;
        wayOfSight_         = (posInLane == 0);

        for (int i = 0; i < totalVehicles_; i++) activeVehicles_.insert(i);

        if (!isGlobalInitialized_) {
            isGlobalInitialized_ = true;
            RaftMetrics::setTotalVehicles(totalVehicles_);
            RaftMetrics::openResultsFile(resultsFileName_);
        }

        timeArrived_ = simTime();
        clusterPhase_ = PHASE_DISCOVERY;

        // Create timers
        checkTimer_        = new cMessage("checkTimer");
        raftPeriodicTimer_ = new cMessage("raftPeriodicTimer");
        discoveryTimer_    = new cMessage("discoveryTimer");

        scheduleAt(simTime() + CHECK_INTERVAL, checkTimer_);
        scheduleAt(simTime() + uniform(0, discoveryBeaconInterval_), discoveryTimer_);
    }
}

void WillemtRaftWaveApplication::finish()
{
    if (!metricsWritten_ && RaftMetrics::isOpen() && hasStoppedAtIntersection_) {
        if (timeStartedMoving_ == SIMTIME_ZERO) timeStartedMoving_ = simTime();
        if (timePassed_ == SIMTIME_ZERO)        timePassed_        = simTime();
        outputMetricsJSON();
    }

    cancelAndDelete(checkTimer_);
    cancelAndDelete(raftPeriodicTimer_);
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

void WillemtRaftWaveApplication::handlePositionUpdate(cObject* obj)
{
    DemoBaseApplLayer::handlePositionUpdate(obj);
    if (!mobility_) {
        mobility_ = TraCIMobilityAccess().get(getParentModule());
        if (mobility_) {
            traci_        = mobility_->getCommandInterface();
            traciVehicle_ = mobility_->getVehicleCommandInterface();
        }
    }
}

// ============ SELF MESSAGE HANDLING ============

void WillemtRaftWaveApplication::handleSelfMsg(cMessage* msg)
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
        // Broadcast a beacon and check if we should form a cluster
        if (clusterPhase_ == PHASE_DISCOVERY) {
            sendDiscoveryBeacon();
            checkClusterTrigger();
        } else if (clusterPhase_ == PHASE_COORDINATION && !hasPassedIntersection_) {
            sendDiscoveryBeacon();
            broadcastClusterExists();
        }
        scheduleAt(simTime() + uniform(0, discoveryBeaconInterval_), discoveryTimer_);
    }
    else if (msg == raftPeriodicTimer_) {
        processRaftPeriodic();
        if (!hasPassedIntersection_ && !isFallbackMode_)
            scheduleAt(simTime() + RAFT_PERIODIC_INTERVAL, raftPeriodicTimer_);
    }
    else if (strcmp(msg->getName(), "oneshotTimer") == 0) {
        // OneshotMsg is a cMessage subclass that carries a std::function callback.
        // We use dynamic_cast via a locally-defined type trick: since the type is
        // defined in scheduleOneshotMs, we re-define the same layout here.
        // Safer: store the callback index in setKind() and use parallel vector.
        // For correctness we use reinterpret approach via function stored in msg object.
        // The msg was created by scheduleOneshotMs as a cMessage with extra data.
        // We use a static helper cast approach:
        struct OneshotPayload {
            std::function<void()> cb;
        };
        // The OneshotMsg in scheduleOneshotMs has the same memory layout. We delete
        // the msg only AFTER calling the callback to avoid use-after-free.
        // Since we cannot cross-function dynamic_cast a locally defined struct,
        // we use a workaround: store the callback in a static map keyed by msg ptr.
        auto it = oneshotCallbacks_.find(msg);
        if (it != oneshotCallbacks_.end()) {
            std::cout << std::fixed << std::setprecision(1) << (simTime().dbl()*1000.0)
                      << "ms Vehicle " << myId_ << " EXECUTING oneshot cb" << std::endl;
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

void WillemtRaftWaveApplication::onWSM(BaseFrame1609_4* frame)
{
    if (hasPassedIntersection_) return;

    benchmark::RaftWaveMessage* wsm = dynamic_cast<benchmark::RaftWaveMessage*>(frame);
    if (!wsm) return;

    int msgType  = wsm->getMsgType();
    int senderId = wsm->getSenderId();
    int targetId = wsm->getTargetId();

    if (targetId != -1 && targetId != myId_) return;

    // Extract payload bytes
    std::vector<uint8_t> payload;
    unsigned int pLen = wsm->getPayloadLen();
    payload.resize(pLen);
    for (unsigned int i = 0; i < pLen; i++) payload[i] = wsm->getPayload(i);

    messagesReceived_++;

    switch (msgType) {
        case benchmark::DISCOVERY_BEACON: {
            uint8_t senderPhase = (payload.size() >= sizeof(int)+1) ? payload[sizeof(int)] : 0;
            if (senderPhase == PHASE_DISCOVERY)
                handleDiscoveryBeacon(senderId, senderPhase);
            else
                discoveredPeers_.erase(senderId);
            break;
        }
        case benchmark::CLUSTER_FORM:
            handleClusterForm(payload, senderId);
            break;
        case benchmark::CLUSTER_EXISTS:
            handleClusterExists(payload, senderId);
            break;
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
        case benchmark::COORD_STATUS_REQUEST:
            handleStatusRequest(senderId);
            break;
        case benchmark::COORD_STATUS_RESPONSE:
            if (payload.size() >= sizeof(VehicleProposal)) {
                VehicleProposal proposal;
                memcpy(&proposal, payload.data(), sizeof(VehicleProposal));
                handleStatusResponseProposal(senderId, proposal);
            }
            break;
        case benchmark::COORD_VEHICLE_PASSED:
            if (payload.size() >= 1) handleVehiclePassed((int)payload[0]);
            break;
        case benchmark::COORD_VEHICLE_LEFT:
        case benchmark::COORD_VEHICLE_LEFT_REBROADCAST:
            if (payload.size() >= sizeof(VehicleLeftEntry)) {
                VehicleLeftEntry e;
                memcpy(&e, payload.data(), sizeof(e));
                handleVehicleLeft(e.vehicleId);
            } else if (payload.size() >= 1) {
                handleVehicleLeft((int)payload[0]);
            }
            break;
    }
}

// ============ WAVE RAFT MESSAGE SEND HELPERS ============

void WillemtRaftWaveApplication::sendRaftMessage(int msgType, int targetId,
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

void WillemtRaftWaveApplication::broadcastRaftMessage(int msgType, const std::vector<uint8_t>& data)
{
    sendRaftMessage(msgType, -1, data);
}

// ============ TRANSPORT IMPLEMENTATION (RaftAppBase pure virtuals) ============

void WillemtRaftWaveApplication::sendRaftUnicast(int targetVehicleId, int msgType,
                                                  const std::vector<uint8_t>& data)
{
    sendRaftMessage(msgType, targetVehicleId, data);
}

void WillemtRaftWaveApplication::sendRaftBroadcast(int msgType, const std::vector<uint8_t>& data)
{
    sendRaftMessage(msgType, -1, data);
}

double WillemtRaftWaveApplication::getDistanceToJunction() const
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

void WillemtRaftWaveApplication::onClusterFormed()
{
    // WAVE requires explicit starting of the periodic RAFT processing
    if (raftPeriodicTimer_ && !raftPeriodicTimer_->isScheduled()) {
        scheduleAt(simTime() + RAFT_PERIODIC_INTERVAL, raftPeriodicTimer_);
    }
}

double WillemtRaftWaveApplication::getRandomDouble(double lo, double hi)
{
    return uniform(lo, hi);
}

void WillemtRaftWaveApplication::scheduleOneshotMs(double delayMs, std::function<void()> fn)
{
    cMessage* tmsg = new cMessage("oneshotTimer");
    std::cout << std::fixed << std::setprecision(1) << (simTime().dbl()*1000.0) 
              << "ms Vehicle " << myId_ << " scheduling oneshot IN " << delayMs << "ms" << std::endl;
    oneshotCallbacks_[tmsg] = fn;
    oneshotTimers_.push_back(tmsg);
    scheduleAt(simTime() + SimTime(delayMs / 1000.0), tmsg);
}

// ============ WAVE-SPECIFIC RAFT MESSAGE HANDLERS ============

void WillemtRaftWaveApplication::handleRequestVote(const std::vector<uint8_t>& data, int senderId)
{
    msg_requestvote_t msg;
    deserializeRequestVote(data, &msg);
    msg_requestvote_response_t resp;
    raft_recv_requestvote(raftServer_, raft_get_node(raftServer_, senderId+1), &msg, &resp);
    auto respData = serializeRequestVoteResponse(&resp);
    sendRaftMessage(benchmark::RAFT_REQUEST_VOTE_RESPONSE, senderId, respData);
}

void WillemtRaftWaveApplication::handleRequestVoteResponse(const std::vector<uint8_t>& data, int senderId)
{
    msg_requestvote_response_t msg;
    deserializeRequestVoteResponse(data, &msg);
    raft_recv_requestvote_response(raftServer_, raft_get_node(raftServer_, senderId+1), &msg);
}

void WillemtRaftWaveApplication::handleAppendEntries(const std::vector<uint8_t>& data, int senderId)
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

void WillemtRaftWaveApplication::handleAppendEntriesResponse(const std::vector<uint8_t>& data, int senderId)
{
    msg_appendentries_response_t msg;
    deserializeAppendEntriesResponse(data, &msg);
    raft_recv_appendentries_response(raftServer_, raft_get_node(raftServer_, senderId+1), &msg);
}
