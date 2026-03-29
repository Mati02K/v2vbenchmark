// RaftWaveMessage.msg - Custom WAVE message for RAFT protocol
// Extends BaseFrame1609_4 with binary payload support.
// Enum types live in RaftTypes.msg (shared with UDP).

cplusplus {{
#include "veins/modules/messages/BaseFrame1609_4_m.h"
#include "RaftTypes_m.h"
}}

import veins.modules.messages.BaseFrame1609_4;
import RaftTypes;

namespace benchmark;

// RAFT WAVE message with binary payload
packet RaftWaveMessage extends veins::BaseFrame1609_4 {
    int msgType @enum(RaftMsgType);  // Message type
    int senderId;                     // Sender vehicle ID
    int targetId = -1;                // Target vehicle (-1 for broadcast)
    unsigned int payloadLen;          // Payload length
    char payload[];                   // Serialized RAFT message data
}
