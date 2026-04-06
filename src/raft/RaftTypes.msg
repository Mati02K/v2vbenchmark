// RaftTypes.msg — shared message type enum, no framework dependency.
// Both UdpRaftApplication and WaveRaftApplication use these constants.

namespace benchmark;

enum RaftMsgType {
    // Discovery
    PEER_BEACON                    = 16;   // 0x10
    CLUSTER_JOIN_INVITE            = 21;   // 0x15
    CLUSTER_FORM_BROADCAST         = 22;   // 0x16 — broadcast member list so all members call formCluster()

    // RAFT Core
    RAFT_REQUEST_VOTE              = 32;   // 0x20
    RAFT_REQUEST_VOTE_RESPONSE     = 33;   // 0x21
    RAFT_APPEND_ENTRIES            = 34;   // 0x22
    RAFT_APPEND_ENTRIES_RESPONSE   = 35;   // 0x23

    // Coordination
    COORD_STATUS_REQUEST           = 48;   // 0x30
    COORD_STATUS_RESPONSE          = 49;   // 0x31
    COORD_VEHICLE_PASSED           = 51;   // 0x33
    COORD_VEHICLE_LEFT             = 52;   // 0x34
    COORD_PASS_ORDER_BROADCAST     = 53;   // 0x35

    // Quorum Certificate (multi-round RAFT)
    QC_SIGN_REQUEST                = 54;   // 0x36 — leader asks cluster members to sign committed schedule
    QC_SIGN_RESPONSE               = 55;   // 0x37 — member returns its signature
    QC_BROADCAST                   = 56;   // 0x38 — leader broadcasts assembled QC to all vehicles
}
