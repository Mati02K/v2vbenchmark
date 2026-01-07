#include "HelloWaveApplication.h"
#include <iostream>

Define_Module(HelloWaveApplication);

void HelloWaveApplication::initialize(int stage)
{
    DemoBaseApplLayer::initialize(stage);

    if (stage == 0) {
        myId = getParentModule()->getIndex();

        helloEvent = new cMessage("helloTimer");

        ackedSet.clear();
        ackedSet.insert(myId);
        ackSentTo.clear();

        // clear & cleanup in case
        for (auto& kv : ackTimers) { cancelAndDelete(kv.second); }
        ackTimers.clear();

        stopSendingHello = false;

        helloAttempts = 0;
        startTime = simTime();
        endTime = -1;

        // Keep init logs in EV only
        EV << simTime() << " V" << myId
           << " init acked=" << setToString(ackedSet)
           << " pending=" << pendingAckToString() << "\n";
    }
}

void HelloWaveApplication::handlePositionUpdate(cObject* obj)
{
    DemoBaseApplLayer::handlePositionUpdate(obj);

    if (!helloEvent->isScheduled() && !stopSendingHello) {
        simtime_t delay = uniform(initMin, initMax);
        scheduleHello(delay);

        EV << simTime() << " V" << myId
           << " first pos update -> schedule HELLO in " << delay << "s\n";
    }
}

void HelloWaveApplication::handleSelfMsg(cMessage* msg)
{
    if (msg == helloEvent) {
        sendHello();

        if (!stopSendingHello) {
            int missing = TOTAL_VEHICLES - (int)ackedSet.size();

            // Adaptive period to reduce congestion when only a few ACKs are missing
            simtime_t period = basePeriod;
            if (missing == 1) period = SimTime(0.5);   // slow down a lot
            else if (missing == 2) period = SimTime(0.2);

            simtime_t nextDelay = period + uniform(-jitter, jitter);
            if (nextDelay < SimTime(0)) nextDelay = SimTime(0); // guard
            scheduleHello(nextDelay);

            EV << simTime() << " V" << myId
               << " next HELLO in " << nextDelay << "s (missing=" << missing << ")\n";
        }
        return;
    }

    // Handle delayed ACK timers
    for (auto it = ackTimers.begin(); it != ackTimers.end(); ++it) {
        if (it->second == msg) {
            int targetId = it->first;  // senderId of the HELLO we are ACKing

            sendAck(targetId);

            delete msg;
            ackTimers.erase(it);
            return;
        }
    }

    DemoBaseApplLayer::handleSelfMsg(msg);
}

void HelloWaveApplication::onWSM(BaseFrame1609_4* wsm)
{
    std::string msgName = wsm->getName();

    if (msgName.rfind("HELLO", 0) == 0) {
        processHello(wsm);
    }
    else if (msgName.rfind("ACK", 0) == 0) {
        processAck(wsm);
    }
}

void HelloWaveApplication::scheduleHello(simtime_t delay)
{
    scheduleAt(simTime() + delay, helloEvent);
}

void HelloWaveApplication::sendHello()
{
    if (stopSendingHello) return;

    // Stop condition (everyone acked me)
    if ((int)ackedSet.size() >= TOTAL_VEHICLES) {
        stopSendingHello = true;
        if (helloEvent->isScheduled()) cancelEvent(helloEvent);

        endTime = simTime();
        double duration = (endTime - startTime).dbl();

        // CLEAN stdout: only completion
        std::cout << simTime() << " V" << myId
                  << " COMPLETED attempts=" << helloAttempts
                  << " duration=" << duration << "s"
                  << " acked=" << ackedSet.size() << "/" << TOTAL_VEHICLES
                  << std::endl;
        return;
    }

    helloAttempts++;

    std::string msgName = "HELLO_from_" + std::to_string(myId);

    BaseFrame1609_4* wsm = new BaseFrame1609_4(msgName.c_str());
    populateWSM(wsm);

    // Broadcast HELLO
    wsm->setRecipientAddress(-1);
    sendDown(wsm);

    std::cout << simTime() << " V" << myId
       << " TX HELLO #" << helloAttempts
       << " acked=" << setToString(ackedSet)
       << " pending=" << pendingAckToString()
       << std::endl;

    EV << simTime() << " V" << myId
       << " TX HELLO #" << helloAttempts
       << " acked=" << setToString(ackedSet)
       << " pending=" << pendingAckToString()
       << "\n";
}

void HelloWaveApplication::sendAck(int targetId)
{
    // ACK is broadcast, target is encoded in name
    std::string msgName =
        "ACK_from_" + std::to_string(myId) + "_to_" + std::to_string(targetId);

    BaseFrame1609_4* wsm = new BaseFrame1609_4(msgName.c_str());
    populateWSM(wsm);

    wsm->setRecipientAddress(-1);
    sendDown(wsm);

    EV << simTime() << " V" << myId
       << " TX ACK(to=" << targetId << ") name=" << msgName << "\n";
}

void HelloWaveApplication::processHello(BaseFrame1609_4* wsm)
{
    int senderId = parseSenderId(wsm->getName());
    if (senderId < 0 || senderId == myId) return;

    // IMPORTANT: ACK each sender only once (prevents ACK storms)
    if (ackSentTo.count(senderId)) return;
    ackSentTo.insert(senderId);

    // Random backoff before ACK to reduce collisions across receivers
    if (!ackTimers.count(senderId)) {
        cMessage* t = new cMessage("ackTimer");
        ackTimers[senderId] = t;
        scheduleAt(simTime() + uniform(0, ackBackoffMax), t);
    }

    EV << simTime() << " V" << myId
       << " RX HELLO from " << senderId
       << " -> schedule ACK backoff<= " << ackBackoffMax << "s\n";
}

void HelloWaveApplication::processAck(BaseFrame1609_4* wsm)
{
    std::string msgName = wsm->getName();

    // Parse "ACK_from_X_to_Y"
    size_t toPos = msgName.find("_to_");
    if (toPos == std::string::npos) return;

    int targetId = -1;
    try {
        targetId = std::stoi(msgName.substr(toPos + 4));
    } catch (...) {
        return;
    }

    // Not for me -> ignore
    if (targetId != myId) return;

    int senderId = parseSenderId(msgName.c_str());
    if (senderId < 0 || senderId == myId) return;

    // Only act when new
    if (ackedSet.find(senderId) == ackedSet.end()) {
        ackedSet.insert(senderId);

        EV << simTime() << " V" << myId
           << " RX ACK from " << senderId
           << " acked=" << setToString(ackedSet)
           << " pending=" << pendingAckToString()
           << "\n";

        // If everyone acked me, stop sending (completion will be printed by next sendHello() check
        // BUT we can also complete immediately here for faster log)
        if ((int)ackedSet.size() >= TOTAL_VEHICLES && !stopSendingHello) {
            stopSendingHello = true;
            if (helloEvent->isScheduled()) cancelEvent(helloEvent);

            endTime = simTime();
            double duration = (endTime - startTime).dbl();

            std::cout << simTime() << " V" << myId
                      << " COMPLETED attempts=" << helloAttempts
                      << " duration=" << duration << "s"
                      << " acked=" << ackedSet.size() << "/" << TOTAL_VEHICLES
                      << std::endl;
        }
    }
}

int HelloWaveApplication::parseSenderId(const char* name) const
{
    std::string s(name);

    // Works for both:
    // "HELLO_from_X"
    // "ACK_from_X_to_Y"
    size_t pos = s.find("from_");
    if (pos == std::string::npos) return -1;

    size_t start = pos + 5;
    size_t end = s.find("_", start);

    std::string idStr = (end == std::string::npos) ? s.substr(start)
                                                   : s.substr(start, end - start);

    try {
        return std::stoi(idStr);
    } catch (...) {
        return -1;
    }
}

std::string HelloWaveApplication::setToString(const std::set<int>& s) const
{
    std::string out = "{";
    bool first = true;
    for (int x : s) {
        if (!first) out += ",";
        out += std::to_string(x);
        first = false;
    }
    out += "}";
    return out;
}

std::string HelloWaveApplication::pendingAckToString() const
{
    std::string out = "{";
    bool first = true;
    for (int id = 0; id < TOTAL_VEHICLES; id++) {
        if (ackedSet.find(id) == ackedSet.end()) {
            if (!first) out += ",";
            out += std::to_string(id);
            first = false;
        }
    }
    out += "}";
    return out;
}

void HelloWaveApplication::finish()
{
    // Optional: print a clean timeout summary if not completed
    if ((int)ackedSet.size() < TOTAL_VEHICLES) {
        std::cout << simTime() << " V" << myId
                  << " TIMEOUT attempts=" << helloAttempts
                  << " acked=" << ackedSet.size() << "/" << TOTAL_VEHICLES
                  << " pending=" << pendingAckToString()
                  << std::endl;
    }

    if (helloEvent) {
        cancelAndDelete(helloEvent);
        helloEvent = nullptr;
    }

    for (auto& kv : ackTimers) {
        cancelAndDelete(kv.second);
    }
    ackTimers.clear();

    DemoBaseApplLayer::finish();
}
