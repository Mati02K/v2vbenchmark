#include "raft/ChannelMetrics.h"

ChannelMetrics::ChannelMetrics(int vehicleId, const std::string& csvPath)
    : vehicleId_(vehicleId)
    , csv_(nullptr)
    , busyAccum_(SIMTIME_ZERO)
    , busyStart_(SIMTIME_ZERO)
{
    csv_ = fopen(csvPath.c_str(), "w");
    if (csv_) {
        fprintf(csv_, "time_s,vehicle_id,channel_utilization\n");
        fflush(csv_);
    }
}

ChannelMetrics::~ChannelMetrics()
{
    if (csv_) {
        fclose(csv_);
        csv_ = nullptr;
    }
}

void ChannelMetrics::receiveSignal(cComponent*, simsignal_t, bool isBusy, cObject*)
{
    simtime_t now = simTime();
    if (isBusy && !channelBusy_) {
        busyStart_ = now;
        channelBusy_ = true;
    } else if (!isBusy && channelBusy_) {
        busyAccum_ += now - busyStart_;
        channelBusy_ = false;
    }
}

void ChannelMetrics::tick(simtime_t now)
{
    simtime_t accum = busyAccum_;
    if (channelBusy_) {
        accum += now - busyStart_;
    }

    double utilization = accum.dbl() / UTILIZATION_WINDOW_SEC;
    if (utilization > 1.0) utilization = 1.0;

    if (csv_) {
        fprintf(csv_, "%.3f,%d,%.4f\n", now.dbl(), vehicleId_, utilization);
        fflush(csv_);
    }

    // Reset for next 100 ms window; if still busy, restart accumulation from now.
    busyAccum_ = SIMTIME_ZERO;
    if (channelBusy_) {
        busyStart_ = now;
    }
}
