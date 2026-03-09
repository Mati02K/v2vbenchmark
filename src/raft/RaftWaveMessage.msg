// RaftWaveMessage.msg - Custom WAVE message for RAFT protocol
// Extends BaseFrame1609_4 with binary payload support

cplusplus {{
#include "veins/modules/messages/BaseFrame1609_4_m.h"
}}

import veins.modules.messages.BaseFrame1609_4;

namespace benchmark;

// Message types for RAFT protocol
enum RaftMsgType {
    // Discovery: each vehicle broadcasts its own VehicleProposal
    PEER_BEACON                    = 16;  // 0x10

    // Intersection leader exchange: lane leader broadcasts full vehicleDB_
    LEADER_DB_EXCHANGE             = 20;  // 0x14

    // Cluster join invitation: lane leader unicasts RAFT member list to a vehicle
    CLUSTER_JOIN_INVITE            = 21;  // 0x15

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
}

// RAFT WAVE message with binary payload
packet RaftWaveMessage extends veins::BaseFrame1609_4 {
    int msgType @enum(RaftMsgType);  // Message type
    int senderId;                     // Sender vehicle ID
    int targetId = -1;                // Target vehicle (-1 for broadcast)
    unsigned int payloadLen;          // Payload length
    char payload[];                   // Serialized RAFT message data
}
