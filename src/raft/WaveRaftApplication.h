#pragma once
// WaveRaftApplication.h — WAVE/802.11p transport subclass of RaftAppBase
// Only contains WAVE-specific transport glue; all shared logic is in RaftAppBase.

#include "veins/modules/application/ieee80211p/DemoBaseApplLayer.h"
#include "veins/modules/mobility/traci/TraCIMobility.h"
#include "veins/modules/mobility/traci/TraCICommandInterface.h"
#include "raft/RaftAppBase.h"
#include <map>

using namespace veins;

class WaveRaftApplication
    : public DemoBaseApplLayer
    , public RaftAppBase
{
public:
    WaveRaftApplication();
    virtual ~WaveRaftApplication();

protected:
    // OMNeT++ application lifecycle (Veins)
    virtual void initialize(int stage) override;
    virtual void finish() override;
    virtual void onWSM(BaseFrame1609_4* wsm) override;
    virtual void handleSelfMsg(cMessage* msg) override;
    virtual void handlePositionUpdate(cObject* obj) override;

private:
    // WAVE-specific mobility pointer
    TraCIMobility* mobility_ = nullptr;

    // WAVE-specific junction position (set at initialise stage 2)
    double junctionX_ = 0, junctionY_ = 0;
    bool   junctionPosKnown_ = false;

    // WAVE-specific cMessage timers
    cMessage* checkTimer_         = nullptr;
    cMessage* raftPeriodicTimer_  = nullptr;
    cMessage* closeResultsFileTimer_ = nullptr;  // Force-close JSON at sim end (avoids truncation)
    cMessage* discoveryTimer_     = nullptr;
    cMessage* statusTimeoutTimer_ = nullptr;
    cMessage* passOrderTimer_     = nullptr;
    cMessage* fallbackTimer_      = nullptr;
    cMessage* arrivalWaitTimer_   = nullptr;

    // One-shot timer registry (used by scheduleOneshotMs)
    std::vector<cMessage*> oneshotTimers_;
    std::map<cMessage*, std::function<void()>> oneshotCallbacks_;

    // Global single-init guard
    static bool isGlobalInitialized_;

    // ── WAVE-specific RAFT message wrappers ────────────────────────────
    void sendRaftMessage(int msgType, int targetId, const std::vector<uint8_t>&);
    void broadcastRaftMessage(int msgType, const std::vector<uint8_t>&);

    // ── WAVE WSM handlers (call base class methods internally) ─────────
    void handleRequestVote(const std::vector<uint8_t>&, int senderId);
    void handleRequestVoteResponse(const std::vector<uint8_t>&, int senderId);
    void handleAppendEntries(const std::vector<uint8_t>&, int senderId);
    void handleAppendEntriesResponse(const std::vector<uint8_t>&, int senderId);

    // ── RaftAppBase pure-virtual implementations ───────────────────────
    void sendRaftToPeer(int targetVehicleId,
                         int msgType,
                         const std::vector<uint8_t>& data) override;
    virtual void sendRaftBroadcast(int msgType, const std::vector<uint8_t>& data) override;
    virtual double getDistanceToJunction() const override;
    virtual void scheduleOneshotMs(double delayMs, std::function<void()> fn) override;
    virtual void onClusterFormed() override;
    double getRandomDouble(double lo, double hi) override;

protected:    // ── parseEdgeParameters (reads OMNeT++ par() — must stay in subclass) ──
    void parseEdgeParametersFromNed();
};
