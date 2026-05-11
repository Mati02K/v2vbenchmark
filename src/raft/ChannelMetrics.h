#pragma once
#include <omnetpp.h>
#include <string>
#include <cstdio>
#include <limits>

using namespace omnetpp;

// Per-vehicle channel utilization + SINR logger.
// Subscribes to Mac1609_4::sigChannelBusy for utilization.
// addSinrSample() is called per received packet from WaveRaftApplication::onWSM().
// tick() flushes one CSV row per 100 ms window to both files.
class ChannelMetrics : public cListener {
public:
    ChannelMetrics(int vehicleId,
                   const std::string& utilizationCsvPath,
                   const std::string& sinrCsvPath);
    ~ChannelMetrics();

    // Called by OMNeT++ signal subsystem when Mac1609_4 emits sigChannelBusy.
    void receiveSignal(cComponent* src, simsignal_t sig, bool isBusy, cObject* detail) override;

    // Called per received RAFT/CAM packet with its linear SINR value from DeciderResult80211.
    void addSinrSample(double sinr_linear);

    // Called every 100 ms by the owning WaveRaftApplication timer.
    void tick(simtime_t now);

private:
    int vehicleId_;

    // Utilization CSV
    FILE* csv_     = nullptr;
    simtime_t busyAccum_;
    simtime_t busyStart_;
    bool channelBusy_ = false;

    // SINR CSV — mean and min SINR (dB) per 100 ms window
    FILE* sinrCsv_ = nullptr;
    double sinrSum_     = 0.0;
    double sinrMin_     = std::numeric_limits<double>::infinity();
    int    sinrSamples_ = 0;

    static constexpr double UTILIZATION_WINDOW_SEC = 0.100;
};
