// RaftWaveMessage.msg - Custom WAVE message for RAFT protocol
// Extends BaseFrame1609_4 with binary payload support

cplusplus {{
#include "veins/modules/messages/BaseFrame1609_4_m.h"
}}

import veins.modules.messages.BaseFrame1609_4;

namespace benchmark;

// Message types for RAFT protocol
enum RaftMsgType {
    RAFT_REQUEST_VOTE = 1;
    RAFT_REQUEST_VOTE_RESPONSE = 2;
    RAFT_APPEND_ENTRIES = 3;
    RAFT_APPEND_ENTRIES_RESPONSE = 4;
    COORD_STATUS_REQUEST = 10;
    COORD_STATUS_RESPONSE = 11;
    COORD_VEHICLE_PASSED = 12;
    COORD_VEHICLE_LEFT = 13;
    COORD_VEHICLE_LEFT_REBROADCAST = 14;
}

// RAFT WAVE message with binary payload
packet RaftWaveMessage extends veins::BaseFrame1609_4 {
    int msgType @enum(RaftMsgType);  // Message type
    int senderId;                     // Sender vehicle ID
    int targetId = -1;                // Target vehicle (-1 for broadcast)
    unsigned int payloadLen;          // Payload length
    char payload[];                   // Serialized RAFT message data
}
