#pragma once
// UdpRaftApplication.h — UDP/INET transport subclass of RaftAppBase
// Only contains UDP-specific transport glue; all shared logic is in RaftAppBase.

#include "veins_inet/VeinsInetApplicationBase.h"
#include "veins_inet/VeinsInetMobility.h"
#include "raft/RaftAppBase.h"

using namespace inet;

class UdpRaftApplication
    : public veins::VeinsInetApplicationBase
    , public RaftAppBase
{
public:
    UdpRaftApplication();
    virtual ~UdpRaftApplication();

protected:
    // OMNeT++ application lifecycle (INET)
    virtual bool startApplication() override;
    virtual bool stopApplication()  override;
    virtual void finish() override;
    virtual void processPacket(std::shared_ptr<inet::Packet> pk) override;

private:
    // UDP-specific mobility pointer
    veins::VeinsInetMobility* mobility_ = nullptr;

    // Timers
    omnetpp::cMessage* checkTimer_{nullptr};
    omnetpp::cMessage* discoveryTimer_{nullptr};
    omnetpp::cMessage* closeResultsFileTimer_{nullptr};
    omnetpp::cMessage* raftPeriodicTimer_{nullptr};

    std::vector<omnetpp::cMessage*> oneshotTimers_;
    std::map<omnetpp::cMessage*, std::function<void()>> oneshotCallbacks_;

    virtual void handleMessageWhenUp(omnetpp::cMessage* msg) override;

    // ── UDP RAFT message handlers ──────────────────────────────────────
    void handleRequestVote(const std::vector<uint8_t>&, const std::string& pktName);
    void handleRequestVoteResponse(const std::vector<uint8_t>&, const std::string& pktName);
    void handleAppendEntries(const std::vector<uint8_t>&, const std::string& pktName);
    void handleAppendEntriesResponse(const std::vector<uint8_t>&, const std::string& pktName);

    // ── Packet-name helpers ────────────────────────────────────────────
    int extractTargetFromPacketName(const std::string&);
    int extractSenderFromPacketName(const std::string&);

    // ── RaftAppBase pure-virtual implementations ───────────────────────
    void sendRaftToPeer(int targetVehicleId,
                         int msgType,
                         const std::vector<uint8_t>& data) override;
    void sendRaftBroadcast(int msgType, const std::vector<uint8_t>& data) override;
    double getDistanceToJunction() const override;
    void scheduleOneshotMs(double delayMs, std::function<void()> fn) override;
    void onClusterFormed() override;
    double getRandomDouble(double lo, double hi) override;

    // ── parseEdgeParameters (reads OMNeT++ par() — must stay in subclass) ──
    void parseEdgeParametersFromNed();
};
