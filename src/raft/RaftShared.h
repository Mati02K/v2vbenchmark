#pragma once
// RaftShared.h — Shared structs, enums and constants for RAFT intersection applications
// Included by both WillemtRaftApplication (UDP) and WillemtRaftWaveApplication (WAVE)

#include <cstdint>
#include <cstring>
#include <string>

extern "C" {
#include "../../third_party/raft/raft_types.h"
}

// ============ LOG ENTRY TYPES ============

enum LogEntryType : uint8_t {
    PASS_COMMAND  = 1,   // Legacy
    STATUS_REPORT = 2,
    PASS_ORDER    = 3,
    VEHICLE_LEFT  = 4
};

// ============ SHARED DATA STRUCTS ============

struct VehicleStatus {
    int  vehicleId;
    bool wayOfSight;
    char lane[64];
    int  positionInLane;
    int  direction; // 0=Straight, 1=Left, 2=Right
};

struct StatusReportEntry {
    int           numVehicles;
    VehicleStatus statuses[32];
};

struct VehicleProposal {
    int    vehicleId;
    char   laneEdgeId[64];
    double positionOnLane;
    double speed;
    int    laneIndex;        // Direction: 0=W, 1=S, 2=E, 3=N
    int    intendedTurn;     // 0=STRAIGHT, 1=LEFT, 2=RIGHT
    bool   isFirstInLane;
    int    blockedByVehicleId;  // -1 = none
    double waitingTimeMs;
    double distanceToJunction;
};

struct PassCommandEntry {
    int        vehicleId;
    double     proposedTime; // simtime_t stored as double
};

struct PassBatch {
    int numVehicles;
    int vehicleIds[8];
};

struct PassScheduleEntry {
    int       numBatches;
    PassBatch batches[16];
};

struct PassOrderEntry {
    int numVehicles;
    int order[32];
};

struct VehicleLeftEntry {
    int vehicleId;
    int batchId;  // unified field name (WAVE used .batch, UDP used .batchId — now unified)
};
