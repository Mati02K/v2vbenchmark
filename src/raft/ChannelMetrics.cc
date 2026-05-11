#include "raft/ChannelMetrics.h"
#include <cmath>

ChannelMetrics::ChannelMetrics(int vehicleId,
                               const std::string& utilizationCsvPath,
                               const std::string& sinrCsvPath)
    : vehicleId_(vehicleId)
    , busyAccum_(SIMTIME_ZERO)
    , busyStart_(SIMTIME_ZERO)
{
    csv_ = fopen(utilizationCsvPath.c_str(), "w");
    if (csv_) {
        fprintf(csv_, "time_s,vehicle_id,channel_utilization\n");
        fflush(csv_);
    }

    sinrCsv_ = fopen(sinrCsvPath.c_str(), "w");
    if (sinrCsv_) {
        fprintf(sinrCsv_, "time_s,vehicle_id,sinr_db_mean,sinr_db_min\n");
        fflush(sinrCsv_);
    }
}

ChannelMetrics::~ChannelMetrics()
{
    if (csv_)     { fclose(csv_);     csv_     = nullptr; }
    if (sinrCsv_) { fclose(sinrCsv_); sinrCsv_ = nullptr; }
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

void ChannelMetrics::addSinrSample(double sinr_linear)
{
    if (sinr_linear <= 0.0) return;
    double sinr_db = 10.0 * std::log10(sinr_linear);
    sinrSum_ += sinr_db;
    if (sinr_db < sinrMin_) sinrMin_ = sinr_db;
    sinrSamples_++;
}

void ChannelMetrics::tick(simtime_t now)
{
    // ---- Channel utilization ----
    simtime_t accum = busyAccum_;
    if (channelBusy_) accum += now - busyStart_;
    double utilization = accum.dbl() / UTILIZATION_WINDOW_SEC;
    if (utilization > 1.0) utilization = 1.0;
    if (csv_) {
        fprintf(csv_, "%.3f,%d,%.4f\n", now.dbl(), vehicleId_, utilization);
        fflush(csv_);
    }
    busyAccum_ = SIMTIME_ZERO;
    if (channelBusy_) busyStart_ = now;

    // ---- SINR ----
    if (sinrCsv_) {
        if (sinrSamples_ > 0) {
            double mean_db = sinrSum_ / sinrSamples_;
            double min_db  = sinrMin_;
            fprintf(sinrCsv_, "%.3f,%d,%.4f,%.4f\n",
                    now.dbl(), vehicleId_, mean_db, min_db);
        }
        // Write nothing when no packets were received this window (sparse CSV).
        fflush(sinrCsv_);
    }
    sinrSum_     = 0.0;
    sinrMin_     = std::numeric_limits<double>::infinity();
    sinrSamples_ = 0;
}
