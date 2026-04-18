// RaftMetrics.cc — Implementation of centralized metrics and JSON output

#include "raft/RaftMetrics.h"
#include <iostream>
#include <iomanip>
#include <algorithm>

// ============ STATIC MEMBER DEFINITIONS ============

std::ofstream RaftMetrics::resultsFile_;
bool          RaftMetrics::resultsFileOpened_    = false;
int           RaftMetrics::vehiclesCompleted_    = 0;
int           RaftMetrics::totalVehiclesStatic_  = 4;
std::string   RaftMetrics::resultsFileNameStatic_ = "raft_results.json";

// ============ FILE MANAGEMENT ============

void RaftMetrics::openResultsFile(const std::string& filename)
{
    if (!resultsFileOpened_) {
        resultsFile_.open(filename, std::ios::out | std::ios::trunc);
        if (resultsFile_.is_open()) {
            resultsFile_ << "[\n";
            resultsFileOpened_    = true;
            resultsFileNameStatic_ = filename;
            std::cout << "Opened results file: " << filename << std::endl;
        } else {
            std::cerr << "ERROR: Failed to open results file: " << filename << std::endl;
        }
    }
}

void RaftMetrics::closeResultsFile()
{
    if (resultsFileOpened_) {
        resultsFile_ << "\n]";
        resultsFile_.close();
        resultsFileOpened_ = false;
        vehiclesCompleted_ = 0;
        std::cout << "Closed results file." << std::endl;
    }
}

// ============ JSON OUTPUT ============

void RaftMetrics::writeVehicleJSON(
    int vehicleId,
    const std::string& lane,
    const std::string& route,
    bool wasLeader,
    bool isPriorityVehicle,
    const std::string& coordinationMethod,
    const std::string& transport,
    double stoppedMs,
    double clusterFormedMs,
    double electedMs,
    double orderCommittedMs,
    double startedMovingMs,
    double passedMs,
    double raftDecisionTimeMs,
    int messagesSent,
    int messagesReceived,
    int electionRounds,
    int logEntriesProposed,
    int logEntriesCommitted,
    int myBatch,
    const std::vector<int>& clusterMembers,
    const std::string& clusterMode)
{
    if (!resultsFileOpened_) return;

    // Derived metrics
    double totalWaitTime    = (passedMs > 0 && stoppedMs > 0) ? (passedMs - stoppedMs) : 0;
    double transitTime  = (passedMs > 0 && startedMovingMs > 0) ? (passedMs - startedMovingMs) : 0;
    double throughput   = 0;  // intersection-level metric, computed post-hoc from aggregate stats

    if (vehiclesCompleted_ > 0) {
        resultsFile_ << ",\n";
    }

    resultsFile_ << "  {\n";
    resultsFile_ << "    \"vehicle_id\": "          << vehicleId << ",\n";
    resultsFile_ << "    \"lane\": \""               << lane << "\",\n";
    resultsFile_ << "    \"route\": \""              << route << "\",\n";
    resultsFile_ << "    \"was_leader\": "              << (wasLeader ? "true" : "false") << ",\n";
    resultsFile_ << "    \"is_priority_vehicle\": "    << (isPriorityVehicle ? "true" : "false") << ",\n";
    resultsFile_ << "    \"coordination_method\": \""  << coordinationMethod << "\",\n";
    resultsFile_ << "    \"transport\": \""          << transport << "\",\n";
    resultsFile_ << "    \"cluster_mode\": \""       << clusterMode << "\",\n";
    resultsFile_ << "    \"timestamps_ms\": {\n";
    resultsFile_ << "      \"stopped\": "            << std::fixed << std::setprecision(1) << stoppedMs << ",\n";
    resultsFile_ << "      \"cluster_formed\": "     << clusterFormedMs << ",\n";
    resultsFile_ << "      \"elected\": "            << electedMs << ",\n";
    resultsFile_ << "      \"order_committed\": "    << orderCommittedMs << ",\n";
    resultsFile_ << "      \"started_moving\": "     << startedMovingMs << ",\n";
    resultsFile_ << "      \"passed\": "             << passedMs << "\n";
    resultsFile_ << "    },\n";
    resultsFile_ << "    \"durations_ms\": {\n";
    resultsFile_ << "      \"raft_decision_time\": " << raftDecisionTimeMs << ",\n";
    resultsFile_ << "      \"total_wait_time\": "    << totalWaitTime << ",\n";
    resultsFile_ << "      \"transit_time\": "       << transitTime << ",\n";
    resultsFile_ << "      \"throughput\": "         << throughput << "\n";
    resultsFile_ << "    },\n";
    resultsFile_ << "    \"messages\": {\n";
    resultsFile_ << "      \"sent\": "               << messagesSent << ",\n";
    resultsFile_ << "      \"received\": "           << messagesReceived << "\n";
    resultsFile_ << "    },\n";
    resultsFile_ << "    \"cluster_size\": "         << clusterMembers.size() << ",\n";
    resultsFile_ << "    \"cluster_members\": [";
    for (size_t i = 0; i < clusterMembers.size(); i++) {
        if (i > 0) resultsFile_ << ", ";
        resultsFile_ << clusterMembers[i];
    }
    resultsFile_ << "],\n";
    resultsFile_ << "    \"raft_stats\": {\n";
    resultsFile_ << "      \"election_rounds\": "    << electionRounds << ",\n";
    resultsFile_ << "      \"entries_proposed\": "   << logEntriesProposed << ",\n";
    resultsFile_ << "      \"entries_committed\": "  << logEntriesCommitted << ",\n";
    resultsFile_ << "      \"my_batch\": "           << myBatch << "\n";
    resultsFile_ << "    }\n";
    resultsFile_ << "  }";
    resultsFile_.flush();

    vehiclesCompleted_++;

    // Auto-close when all vehicles have written
    if (vehiclesCompleted_ >= totalVehiclesStatic_) {
        closeResultsFile();
    }
}
