#include "tcp/HelloTcpApplication.h"
#include <iostream>
#include <vector>
#include "inet/common/packet/Packet.h"
#include "inet/common/packet/chunk/BytesChunk.h"
#include "inet/networklayer/common/L3AddressResolver.h"

using namespace inet;

Define_Module(HelloTcpApplication);

HelloTcpApplication::HelloTcpApplication() {}

HelloTcpApplication::~HelloTcpApplication()
{
    for (auto& kv : clientSockets) {
        delete kv.second;
    }
    for (auto& kv : serverSockets) {
        delete kv.second;
    }
}

bool HelloTcpApplication::startApplication()
{
    myId = getParentModule()->getIndex();

    // GET MOBILITY MODULE
    mobility = check_and_cast<veins::VeinsInetMobility*>(getParentModule()->getSubmodule("mobility"));

    // GET TRACI INTERFACE
    traci = mobility->getCommandInterface();
    traciVehicle = mobility->getVehicleCommandInterface();

    sentHelloTo.clear();
    sentHelloTo.insert(myId);

    connectedPeers.clear();

    stopSending = false;

    helloAttempts = 0;
    connectionAttempts = 0;
    startTime = simTime();

    // Setup TCP server socket to accept incoming connections
    serverSocket.setOutputGate(gate("socketOut"));
    serverSocket.setCallback(this);
    serverSocket.bind(TCP_PORT);
    serverSocket.listen();

    std::cout << simTime() << " Vehicle " << myId
              << " STARTED (TCP mode) listening on port " << TCP_PORT << std::endl;

    // SCHEDULE PERIODIC CHECK FOR INTERSECTION - METHOD 3
    checkPositionHandle = timerManager.create(
        veins::TimerSpecification([this]() {
            checkAndStopAtIntersection();
        }).interval(checkInterval)
    );

    // Start connecting to peers after initial delay
    scheduleConnect(initDelay);

    return true;
}

bool HelloTcpApplication::stopApplication()
{
    if (connectHandle != -1) {
        timerManager.cancel(connectHandle);
        connectHandle = -1;
    }

    if (checkPositionHandle != -1) {
        timerManager.cancel(checkPositionHandle);
        checkPositionHandle = -1;
    }

    serverSocket.close();

    for (auto& kv : clientSockets) {
        kv.second->close();
    }

    for (auto& kv : serverSockets) {
        kv.second->close();
    }

    return true;
}

void HelloTcpApplication::checkAndStopAtIntersection()
{
    if (hasStoppedAtIntersection || !traciVehicle) {
        return;
    }

    try {
        // Get current road/edge ID
        std::string roadId = traciVehicle->getRoadId();

        // Check if vehicle is on one of the intersection edges
        if (INTERSECTION_EDGES.count(roadId) > 0) {
            // Vehicle is at intersection - STOP IT
            traciVehicle->setSpeed(0);
            hasStoppedAtIntersection = true;

            Coord pos = mobility->getCurrentPosition();
            std::cout << simTime() << " Vehicle " << myId
                      << " STOPPED at intersection on edge: " << roadId
                      << " at position " << pos << std::endl;

            // Cancel the position check timer since we've stopped
            if (checkPositionHandle != -1) {
                timerManager.cancel(checkPositionHandle);
                checkPositionHandle = -1;
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Error checking position for Vehicle " << myId
                  << ": " << e.what() << std::endl;
    }
}

void HelloTcpApplication::scheduleConnect(simtime_t delay)
{
    if (stopSending) return;

    connectHandle = timerManager.create(
        veins::TimerSpecification([this]() {
            if (!stopSending) {
                connectToPeers();
                scheduleConnect(connectRetry);
            }
        }).oneshotIn(delay)
    );
}

void HelloTcpApplication::connectToPeers()
{
    connectionAttempts++;

    // Try to connect to all other vehicles
    for (int peerId = 0; peerId < TOTAL_VEHICLES; peerId++) {
        if (peerId == myId) continue;
        if (connectedPeers.count(peerId)) continue;
        if (sentHelloTo.count(peerId)) continue;

        // Check if socket already exists
        if (clientSockets.find(peerId) == clientSockets.end()) {
            // Create new client socket
            TcpSocket* socket = new TcpSocket();
            socket->setOutputGate(gate("socketOut"));
            socket->setCallback(this);

            clientSockets[peerId] = socket;
            socketToPeerId[socket] = peerId;
            socketMap.addSocket(socket);

            // Get peer IP address
            std::string peerIp = PEER_IPS[peerId];
            L3Address peerAddr = L3AddressResolver().resolve(peerIp.c_str());

            std::cout << simTime() << " Vehicle " << myId
                      << " CONNECTING to Vehicle " << peerId
                      << " at " << peerAddr << ":" << TCP_PORT << std::endl;

            socket->connect(peerAddr, TCP_PORT);
        }
    }
}

void HelloTcpApplication::socketAvailable(TcpSocket *socket, TcpAvailableInfo *availableInfo)
{
    // Server socket accepted a new connection
    TcpSocket* newSocket = new TcpSocket(availableInfo);
    newSocket->setOutputGate(gate("socketOut"));
    newSocket->setCallback(this);

    serverSocket.accept(availableInfo->getNewSocketId());

    int connId = nextConnId++;
    serverSockets[connId] = newSocket;
    socketMap.addSocket(newSocket);

    std::cout << simTime() << " Vehicle " << myId
              << " ACCEPTED incoming connection (connId=" << connId << ")" << std::endl;

//    delete availableInfo;
}

void HelloTcpApplication::socketEstablished(TcpSocket *socket)
{
    // Check if this is a client socket we initiated
    auto it = socketToPeerId.find(socket);
    if (it != socketToPeerId.end()) {
        int peerId = it->second;
        connectedPeers.insert(peerId);

        std::cout << simTime() << " Vehicle " << myId
                  << " CONNECTED to Vehicle " << peerId << std::endl;

        // Send HELLO immediately
        sendHelloTcp(peerId, socket);
    }
}

void HelloTcpApplication::sendHelloTcp(int peerId, TcpSocket* socket)
{
    if (stopSending) return;
    if (sentHelloTo.count(peerId)) return;

    helloAttempts++;

    std::string msg = "hello-from-" + std::to_string(myId);
    std::vector<uint8_t> bytes(msg.begin(), msg.end());

    auto chunk = makeShared<BytesChunk>(bytes);
    auto packet = new Packet("tcp-hello");
    packet->insertAtBack(chunk);

    socket->send(packet);

    sentHelloTo.insert(peerId);

    std::cout << simTime() << " Vehicle " << myId
              << " SENDING HELLO #" << helloAttempts
              << " to Vehicle " << peerId << " (TCP)"
              << " | sent " << setToString(sentHelloTo)
              << " | pending " << pendingToString()
              << std::endl;

    // Check completion
    if ((int)sentHelloTo.size() >= TOTAL_VEHICLES) {
        stopSending = true;
        if (connectHandle != -1) {
            timerManager.cancel(connectHandle);
            connectHandle = -1;
        }

        endTime = simTime();
        double duration = (endTime - startTime).dbl();

        std::cout << "============================================" << std::endl;
        std::cout << simTime() << " Vehicle " << myId
                  << " COMPLETED PROTOCOL" << std::endl;
        std::cout << "  Total HELLO attempts: " << helloAttempts << std::endl;
        std::cout << "  Connection attempts: " << connectionAttempts << std::endl;
        std::cout << "  Start time: " << startTime << "s" << std::endl;
        std::cout << "  End time: " << endTime << "s" << std::endl;
        std::cout << "  Duration: " << duration << "s" << std::endl;
        std::cout << "  Sent to: " << setToString(sentHelloTo) << std::endl;
        std::cout << "============================================" << std::endl;
    }
}

void HelloTcpApplication::socketDataArrived(TcpSocket *socket, Packet *packet, bool urgent)
{
    auto chunk = packet->peekDataAt<BytesChunk>(B(0), packet->getDataLength());
    std::vector<uint8_t> bytes = chunk->getBytes();
    std::string msg(bytes.begin(), bytes.end());

    delete packet;

    if (msg.rfind("hello-from-", 0) == 0) {
        try {
            int senderId = std::stoi(msg.substr(11));

            std::cout << simTime() << " Vehicle " << myId
                      << " RECEIVED HELLO from " << senderId
                      << " (TCP)" << std::endl;

        } catch (...) {}
    }
}

void HelloTcpApplication::socketPeerClosed(TcpSocket *socket)
{
    socket->close();
}

void HelloTcpApplication::socketClosed(TcpSocket *socket)
{
    std::cout << simTime() << " Vehicle " << myId
              << " TCP socket CLOSED" << std::endl;
}

void HelloTcpApplication::socketFailure(TcpSocket *socket, int code)
{
    std::cout << simTime() << " Vehicle " << myId
              << " TCP socket FAILURE code=" << code << std::endl;

    // Find which peer this was for
    auto it = socketToPeerId.find(socket);
    if (it != socketToPeerId.end()) {
        int peerId = it->second;
        std::cout << "  -> Failed connection to Vehicle " << peerId << std::endl;

        // Remove from connected set so we can retry
        connectedPeers.erase(peerId);
    }
}

std::string HelloTcpApplication::setToString(const std::set<int>& s) const
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

std::string HelloTcpApplication::pendingToString() const
{
    std::string out = "{";
    bool first = true;
    for (int id = 0; id < TOTAL_VEHICLES; id++) {
        if (sentHelloTo.find(id) == sentHelloTo.end()) {
            if (!first) out += ",";
            out += std::to_string(id);
            first = false;
        }
    }
    out += "}";
    return out;
}
