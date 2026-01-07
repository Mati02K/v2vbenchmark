#pragma once
#include <set>
#include <map>
#include <string>
#include "veins_inet/VeinsInetApplicationBase.h"
#include "inet/transportlayer/contract/tcp/TcpSocket.h"
#include "inet/common/socket/SocketMap.h"
#include "veins_inet/VeinsInetMobility.h"
#include "veins/modules/mobility/traci/TraCICommandInterface.h"

using namespace inet;

class HelloTcpApplication : public veins::VeinsInetApplicationBase, public TcpSocket::ICallback
{
  public:
    HelloTcpApplication();
    virtual ~HelloTcpApplication();

  protected:
    virtual bool startApplication() override;
    virtual bool stopApplication() override;

    // TcpSocket::ICallback methods
    virtual void socketDataArrived(TcpSocket *socket, Packet *packet, bool urgent) override;
    virtual void socketAvailable(TcpSocket *socket, TcpAvailableInfo *availableInfo) override;
    virtual void socketEstablished(TcpSocket *socket) override;
    virtual void socketPeerClosed(TcpSocket *socket) override;
    virtual void socketClosed(TcpSocket *socket) override;
    virtual void socketFailure(TcpSocket *socket, int code) override;
    virtual void socketStatusArrived(TcpSocket *socket, TcpStatusInfo *status) override {}
    virtual void socketDeleted(TcpSocket *socket) override {}

  private:
    // ====== CONFIG ======
    static constexpr int TOTAL_VEHICLES = 4;
    const int TCP_PORT = 9001;
    const simtime_t connectRetry = SimTime(0.5);
    const simtime_t initDelay = SimTime(5.0);
    const simtime_t checkInterval = SimTime(0.1);  // Check position every 0.1s

    // Hardcoded peer IPs (10.0.0.1 to 10.0.0.4)
    const std::vector<std::string> PEER_IPS = {
        "10.0.0.1", "10.0.0.2", "10.0.0.3", "10.0.0.4"
    };

    // Intersection edges from your routes (C2S, C2N, C2E, C2W)
    const std::set<std::string> INTERSECTION_EDGES = {
        "C2S", "C2N", "C2E", "C2W"
    };

    // ====== STATE ======
    int myId = -1;
    bool stopSending = false;

    std::map<int, TcpSocket*> clientSockets;
    std::map<int, TcpSocket*> serverSockets;
    SocketMap socketMap;
    TcpSocket serverSocket;

    std::set<int> connectedPeers;
    std::set<int> sentHelloTo;
    std::map<TcpSocket*, int> socketToPeerId;

    long connectHandle = -1;
    long checkPositionHandle = -1;  // Changed from stopHandle
    int nextConnId = 1000;

    // ====== BENCHMARKING ======
    int helloAttempts = 0;
    int connectionAttempts = 0;
    simtime_t startTime;
    simtime_t endTime;

    // ====== MOBILITY ======
    veins::VeinsInetMobility* mobility = nullptr;
    veins::TraCICommandInterface* traci = nullptr;
    veins::TraCICommandInterface::Vehicle* traciVehicle = nullptr;
    bool hasStoppedAtIntersection = false;

  private:
    void scheduleConnect(simtime_t delay);
    void connectToPeers();
    void sendHelloTcp(int peerId, TcpSocket* socket);
    void checkAndStopAtIntersection();  // NEW METHOD

    std::string setToString(const std::set<int>& s) const;
    std::string pendingToString() const;
};
