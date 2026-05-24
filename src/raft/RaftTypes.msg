// RaftTypes.msg — shared message type enum, no framework dependency.
// Both UdpRaftApplication and WaveRaftApplication use these constants.

namespace benchmark;

enum RaftMsgType {
    // Discovery (1-10)
    PEER_BEACON                    = 1;
    CLUSTER_JOIN_INVITE            = 2;
    CLUSTER_FORM_BROADCAST         = 3;

    // RAFT Core (11-20)
    RAFT_REQUEST_VOTE              = 11;
    RAFT_REQUEST_VOTE_RESPONSE     = 12;
    RAFT_APPEND_ENTRIES            = 13;
    RAFT_APPEND_ENTRIES_RESPONSE   = 14;

    // Coordination (21-30)
    COORD_STATUS_REQUEST           = 21;
    COORD_STATUS_RESPONSE          = 22;
    COORD_VEHICLE_LEFT             = 23;
    COORD_PASS_ORDER_BROADCAST     = 24;

    // Background channel load — not counted in RAFT metrics (99)
    CAM_TELEMETRY                  = 99;
}
