#pragma once
// RaftMetrics.h — Centralized results file management for both RAFT applications
// Replaces the duplicated static file management + JSON output in both .cc files

#include <fstream>
#include <string>
#include <vector>

class RaftMetrics {
public:
    // Open the shared JSON results file (called once, by vehicle 0)
    static void openResultsFile(const std::string& filename);

    // Close the file (called when all vehicles are done)
    static void closeResultsFile();

    // Write one vehicle's metrics as a JSON object into the array
    static void writeVehicleJSON(
        int vehicleId,
        const std::string& lane,
        const std::string& route,
        bool wasLeader,
        const std::string& coordinationMethod,
        const std::string& transport,         // "udp" or "wave"
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
        const std::string& clusterMode
    );

    // Accessors for static state
    static bool isOpen()              { return resultsFileOpened_; }
    static int  vehiclesCompleted()   { return vehiclesCompleted_; }
    static int  totalVehicles()       { return totalVehiclesStatic_; }
    static void setTotalVehicles(int n) { totalVehiclesStatic_ = n; }
    static void incrementCompleted()  { vehiclesCompleted_++; }
    static void setFilename(const std::string& fn) { resultsFileNameStatic_ = fn; }

private:
    static std::ofstream resultsFile_;
    static bool          resultsFileOpened_;
    static int           vehiclesCompleted_;
    static int           totalVehiclesStatic_;
    static std::string   resultsFileNameStatic_;
};
