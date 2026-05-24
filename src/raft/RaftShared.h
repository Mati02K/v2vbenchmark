#pragma once
// RaftShared.h — Shared structs, enums and constants for RAFT intersection applications
// Included by both UdpRaftApplication (UDP) and WaveRaftApplication (WAVE)

#include <cstdint>
#include <cstring>
#include <string>
#include "../../lib/crypto/CryptoAuth.h"  // VehicleCert, SignedProposal

extern "C" {
#include "../../lib/raft/raft_types.h"
}

// ============ SHARED DATA STRUCTS ============

struct VehicleProposal {
    int    vehicleId;
    char   laneEdgeId[64];
    char   sumoId[16];
    double positionOnLane;
    double speed;
    int    laneIndex;           // Direction: 0=W, 1=S, 2=E, 3=N
    int    intendedTurn;        // 0=STRAIGHT, 1=LEFT, 2=RIGHT
    bool   isFirstInLane;
    double waitingTimeMs;
    double distanceToJunction;
    // Set by receiver ONLY after cert verification — never trust from raw payload.
    bool   isPriority;          // true = priority vehicle verified by Emergency_CA
};

struct PassBatch {
    int numVehicles;
    int vehicleIds[8];
};

struct PassScheduleEntry {
    int       numBatches;
    PassBatch batches[16];
};

struct VehicleLeftEntry {
    int vehicleId;
    int batchId;  // unified field name (WAVE used .batch, UDP used .batchId — now unified)
};

