# NuRaft Integration Plan for OMNeT++/Veins

## Overview
NuRaft (eBay) is a production-grade RAFT implementation. To integrate it with OMNeT++, we need to:
1. Create custom RPC client/server that uses OMNeT++ UDP instead of ASIO
2. Implement state machine for intersection coordination
3. Bridge NuRaft's async callbacks to OMNeT++ event system

## Architecture

```
OMNeT++ Vehicle Module
    ↓
RaftIntersectionApplication (wrapper)
    ↓
NuRaft RaftServer (core logic)
    ↓
Custom OMNeT++ RPC Client/Server
    ↓
OMNeT++ UDP Messages
```

## Required Components

### 1. Custom RPC Client (`OmnetRpcClient`)
- Implements `nuraft::rpc_client` interface
- Sends `req_msg` via OMNeT++ UDP
- Receives `resp_msg` via OMNeT++ UDP
- Maps async callbacks to OMNeT++ events

### 2. Custom RPC Listener (`OmnetRpcListener`)
- Implements `nuraft::rpc_listener` interface
- Receives `req_msg` from OMNeT++ UDP
- Forwards to `raft_server::process_req()`
- Sends `resp_msg` back via OMNeT++ UDP

### 3. State Machine (`IntersectionStateMachine`)
- Implements `nuraft::state_machine` interface
- Handles intersection coordination commands
- Applies leader decisions

### 4. State Manager (`OmnetStateManager`)
- Implements `nuraft::state_mgr` interface
- Stores cluster configuration
- Manages server state

## Implementation Steps

1. **Create RPC serialization layer**
   - Serialize `req_msg` to bytes for UDP
   - Deserialize bytes back to `req_msg`
   - Handle `resp_msg` similarly

2. **Implement custom RPC client**
   - Map node IDs to vehicle IDs
   - Send messages via OMNeT++ UDP broadcast
   - Handle responses asynchronously

3. **Implement custom RPC listener**
   - Listen for incoming UDP messages
   - Parse and forward to raft_server
   - Send responses back

4. **Integrate with vehicle application**
   - Initialize NuRaft server on vehicle start
   - Handle leader election callbacks
   - Coordinate intersection crossing

## Challenges

1. **Async to Sync**: NuRaft uses async callbacks, OMNeT++ is event-driven
   - Solution: Use OMNeT++ self-messages to simulate async

2. **Network Layer**: NuRaft expects TCP-like RPC, we have UDP broadcast
   - Solution: Add message IDs and correlation for request/response matching

3. **Timing**: NuRaft uses real-time, OMNeT++ uses simulation time
   - Solution: Map simulation time to NuRaft's internal timers

## Alternative: Simplified Approach

Instead of full NuRaft integration, we could:
- Use NuRaft's core election logic only
- Implement minimal RPC layer
- Focus on leader election, skip log replication (not needed for intersection)

This would be much simpler and faster to implement.
