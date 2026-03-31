#pragma once
// RaftShared.h — Shared structs, enums and constants for RAFT intersection applications
// Included by both UdpRaftApplication (UDP) and WaveRaftApplication (WAVE)

#include <cstdint>
#include <cstring>
#include <string>
#include "../../third_party/crypto/CryptoAuth.h"  // VehicleCert, SignedProposal

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
    int    laneIndex;           // Direction: 0=W, 1=S, 2=E, 3=N
    int    intendedTurn;        // 0=STRAIGHT, 1=LEFT, 2=RIGHT
    bool   isFirstInLane;
    int    blockedByVehicleId;  // -1 = none
    double waitingTimeMs;
    double distanceToJunction;
    // Set by receiver ONLY after cert verification — never trust from raw payload.
    bool   isPriority;          // true = ambulance verified by Emergency_CA
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

// ============ QUORUM CERTIFICATE ============
// Assembled by the RAFT leader after a PASS_ORDER commit.
// Contains a majority of cluster-member signatures over (round || schedule),
// so the next RAFT round can verify the previous schedule without re-running RAFT.
//
// Flow:
//   1. PASS_ORDER commits → leader broadcasts QC_SIGN_REQUEST (round + schedule)
//   2. Each member signs and returns QC_SIGN_RESPONSE
//   3. When leader has majority: assembles QuorumCertificate, broadcasts QC_BROADCAST
//   4. All vehicles (including queued ones) store the QC and populate scheduledVehicles_
//   5. Next-round lane leaders skip scheduledVehicles_ in proposePassOrder()

static constexpr int QC_MAX_MEMBERS = 8;

struct QCSig {
    int     vehicleId;
    uint8_t pubKey[CRYPTO_PUBKEY_BYTES];        // signer's public key (for verification)
    uint8_t sig[CRYPTO_SIG_MAX_BYTES];           // ECDSA sig over [uint32_t round || PassScheduleEntry]
    uint8_t sigLen;
};

struct QuorumCertificate {
    bool              valid;                     // true once assembled with enough sigs
    uint32_t          round;                     // RAFT round number this certifies
    PassScheduleEntry schedule;                  // the committed pass schedule
    int               numSigs;
    QCSig             sigs[QC_MAX_MEMBERS];
};

