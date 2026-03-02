// RaftWaveMessage.msg - Custom WAVE message for RAFT protocol
// Extends BaseFrame1609_4 with binary payload support

cplusplus {{
#include "veins/modules/messages/BaseFrame1609_4_m.h"
}}

import veins.modules.messages.BaseFrame1609_4;

namespace benchmark;

// Message types for RAFT protocol
enum RaftMsgType {
    // Dynamic cluster discovery
    DISCOVERY_BEACON               = 16;  // 0x10 (legacy)
    CLUSTER_FORM                   = 17;  // 0x11
    CLUSTER_EXISTS                 = 18;  // 0x12
    CLUSTER_INVITATION             = 19;  // 0x13 — discovery/merge: (vehicleId, clusterId, timestamp, members)
    
    // RAFT Core
    RAFT_REQUEST_VOTE              = 32;  // 0x20
    RAFT_REQUEST_VOTE_RESPONSE     = 33;  // 0x21
    RAFT_APPEND_ENTRIES            = 34;  // 0x22
    RAFT_APPEND_ENTRIES_RESPONSE   = 35;  // 0x23
    
    // Coordination
    COORD_STATUS_REQUEST           = 48;  // 0x30
    COORD_STATUS_RESPONSE          = 49;  // 0x31
    COORD_VEHICLE_PASSED           = 51;  // 0x33
    COORD_VEHICLE_LEFT             = 52;  // 0x34
    COORD_VEHICLE_LEFT_REBROADCAST = 53;  // 0x35

    // Late joiner: committed cluster sends schedule to vehicles that formed after commit
    LATE_JOIN_ORDER                = 64;  // 0x40
}

// RAFT WAVE message with binary payload
packet RaftWaveMessage extends veins::BaseFrame1609_4 {
    int msgType @enum(RaftMsgType);  // Message type
    int senderId;                     // Sender vehicle ID
    int targetId = -1;                // Target vehicle (-1 for broadcast)
    unsigned int payloadLen;          // Payload length
    char payload[];                   // Serialized RAFT message data
}
