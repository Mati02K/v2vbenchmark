#include "udp/HelloUdpApplication.h"

#include <iostream>
#include <vector>
#include <algorithm>

#include "inet/common/packet/Packet.h"
#include "inet/common/packet/chunk/BytesChunk.h"

using namespace inet;

Define_Module(HelloUdpApplication);

HelloUdpApplication::HelloUdpApplication() {}
HelloUdpApplication::~HelloUdpApplication() {}

bool HelloUdpApplication::startApplication()
{
    myId = getParentModule()->getIndex();

    // Self is considered "acked" immediately (I don't need to ACK myself)
    ackedSet.clear();
    ackedSet.insert(myId);

    stopSendingHello = false;

    // Benchmarking
    helloAttempts = 0;
    startTime = simTime();

    // Initial de-sync
    const double d = uniform(initMin.dbl(), initMax.dbl());
    scheduleHello(SimTime(d));

    return true;
}

bool HelloUdpApplication::stopApplication()
{
    if (helloHandle != -1) {
        timerManager.cancel(helloHandle);
        helloHandle = -1;
    }
    return true;
}

void HelloUdpApplication::scheduleHello(simtime_t delay)
{
    if (stopSendingHello) return;

    helloHandle = timerManager.create(
        veins::TimerSpecification([this]() {
            if (stopSendingHello) return;
            sendHello();

            // Schedule next hello
            const double j = uniform(0.0, jitter.dbl());
            scheduleHello(basePeriod + SimTime(j));
        }).oneshotIn(delay)
    );
}

void HelloUdpApplication::sendHello()
{
    if (stopSendingHello) return;

    // Increment attempt counter
    helloAttempts++;

    // ONLY check ackedSet - stop sending when everyone has ACKed me
    if ((int)ackedSet.size() >= TOTAL_VEHICLES) {
        stopSendingHello = true;
        if (helloHandle != -1) {
            timerManager.cancel(helloHandle);
            helloHandle = -1;
        }
        endTime = simTime();
        double duration = (endTime - startTime).dbl();

        std::cout << "============================================" << std::endl;
        std::cout << simTime() << " Vehicle " << myId
                  << " COMPLETED PROTOCOL" << std::endl;
        std::cout << "  Total HELLO attempts: " << helloAttempts << std::endl;
        std::cout << "  Start time: " << startTime << "s" << std::endl;
        std::cout << "  End time: " << endTime << "s" << std::endl;
        std::cout << "  Duration: " << duration << "s" << std::endl;
        std::cout << "  Acked by: " << setToString(ackedSet) << std::endl;
        std::cout << "============================================" << std::endl;
        return;
    }

    std::string msg = "hello-from-" + std::to_string(myId);

    std::vector<uint8_t> bytes(msg.begin(), msg.end());
    auto payload = makeShared<BytesChunk>(bytes);

    auto packet = createPacket(msg.c_str());
    packet->insertAtBack(payload);

    sendPacket(std::move(packet));

    std::cout << simTime() << " Vehicle " << myId
              << " SENDING HELLO #" << helloAttempts
              << " | acked " << setToString(ackedSet)
              << " | pending ACK " << pendingAckToString()
              << std::endl;
}

void HelloUdpApplication::sendAck(int targetId)
{
    std::string msg = "ack-from-" + std::to_string(myId) + "-to-" + std::to_string(targetId);

    std::vector<uint8_t> bytes(msg.begin(), msg.end());
    auto payload = makeShared<BytesChunk>(bytes);

    auto packet = createPacket(msg.c_str());
    packet->insertAtBack(payload);

    sendPacket(std::move(packet));

    std::cout << simTime() << " Vehicle " << myId
              << " SENDING ACK to " << targetId
              << std::endl;
}

void HelloUdpApplication::processPacket(std::shared_ptr<Packet> pk)
{
    std::string packetName = pk->getName();

    // Determine if it's a HELLO or ACK message
    if (packetName.rfind("hello-from-", 0) == 0) {
        processHello(packetName);
    } else if (packetName.rfind("ack-from-", 0) == 0) {
        processAck(packetName);
    }
}

void HelloUdpApplication::processHello(const std::string& packetName)
{
    int sender = parseSenderId(packetName.c_str());

    if (sender >= 0 && sender != myId) {
        std::cout << simTime() << " Vehicle " << myId
                  << " RECEIVED HELLO from " << sender
                  << std::endl;

        // Always send ACK back when we receive a HELLO
        // (even if we've stopped sending our own HELLOs)
        sendAck(sender);
    }
}

void HelloUdpApplication::processAck(const std::string& packetName)
{
    // Parse "ack-from-X-to-Y"
    // We only care if it's addressed to us (to-Y == myId)
    size_t toPos = packetName.find("-to-");
    if (toPos == std::string::npos) return;

    try {
        int targetId = std::stoi(packetName.substr(toPos + 4));
        if (targetId != myId) return; // Not for us

        size_t fromPos = packetName.find("ack-from-");
        if (fromPos != 0) return;

        std::string middle = packetName.substr(9, toPos - 9);
        int sender = std::stoi(middle);

        bool wasNew = (ackedSet.find(sender) == ackedSet.end());
        ackedSet.insert(sender);

        std::cout << simTime() << " Vehicle " << myId
                  << " RECEIVED ACK from " << sender
                  << " | acked " << setToString(ackedSet)
                  << " | pending ACK " << pendingAckToString()
                  << std::endl;

        // Check if we should stop sending after receiving this ACK
        if ((int)ackedSet.size() >= TOTAL_VEHICLES && !stopSendingHello) {
            stopSendingHello = true;
            if (helloHandle != -1) {
                timerManager.cancel(helloHandle);
                helloHandle = -1;
            }
            endTime = simTime();
            double duration = (endTime - startTime).dbl();

            std::cout << "============================================" << std::endl;
            std::cout << simTime() << " Vehicle " << myId
                      << " COMPLETED PROTOCOL" << std::endl;
            std::cout << "  Total HELLO attempts: " << helloAttempts << std::endl;
            std::cout << "  Start time: " << startTime << "s" << std::endl;
            std::cout << "  End time: " << endTime << "s" << std::endl;
            std::cout << "  Duration: " << duration << "s" << std::endl;
            std::cout << "  Acked by: " << setToString(ackedSet) << std::endl;
            std::cout << "============================================" << std::endl;
        }

    } catch (...) {
        // Parse error, ignore
    }
}

int HelloUdpApplication::parseSenderId(const char* name) const
{
    if (!name) return -1;

    std::string s(name);
    const std::string prefix = "hello-from-";

    if (s.rfind(prefix, 0) != 0) return -1;

    try {
        size_t start = prefix.size();
        return std::stoi(s.substr(start));
    } catch (...) {
        return -1;
    }
}

std::string HelloUdpApplication::setToString(const std::set<int>& s) const
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

std::string HelloUdpApplication::pendingAckToString() const
{
    // pending ACK = all IDs [0..TOTAL_VEHICLES-1] not in ackedSet
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
