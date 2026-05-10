#pragma once
#include <omnetpp.h>
#include <string>
#include <cstdio>

using namespace omnetpp;

// Per-vehicle channel utilization logger (ETSI TS 102 687).
// Subscribes to Mac1609_4::sigChannelBusy and emits one CSV row per 100 ms window.
class ChannelMetrics : public cListener {
public:
    ChannelMetrics(int vehicleId, const std::string& csvPath);
    ~ChannelMetrics();

    // Called by OMNeT++ signal subsystem when Mac1609_4 emits sigChannelBusy.
    void receiveSignal(cComponent* src, simsignal_t sig, bool isBusy, cObject* detail) override;

    // Called every 100 ms by the owning WaveRaftApplication timer.
    void tick(simtime_t now);

private:
    int vehicleId_;
    FILE* csv_;

    simtime_t busyAccum_;
    simtime_t busyStart_;
    bool channelBusy_ = false;

    static constexpr double UTILIZATION_WINDOW_SEC = 0.100;
};
