# RAFT Library Integration Status

## Library Selected: willemt/raft

**Why willemt/raft?**
- Single-header C library (simple to integrate)
- Well-tested, proven RAFT implementation
- No external dependencies
- Easy to bridge to OMNeT++

**Files:**
- `raft.h` - Main RAFT library (single header)
- `raft_types.h` - Type definitions

## Integration Approach

### 1. C++ Wrapper (`WillemtRaftApplication`)
- Wraps the C library in a C++ class
- Implements callbacks as static C functions
- Bridges to OMNeT++ UDP messages

### 2. Message Serialization
- Serialize RAFT messages (requestvote, appendentries) to bytes
- Send via OMNeT++ UDP broadcast
- Deserialize on receive

### 3. Callbacks Implementation
- `send_requestvote` - Send vote requests via UDP
- `send_appendentries` - Send heartbeats via UDP  
- `log_offer` - Handle log entries (minimal for intersection)
- `log_apply` - Apply log entries
- `persist_vote` - Save vote (in-memory for simulation)

### 4. Timing
- Use `raft_periodic()` every 50ms to handle timeouts
- Map simulation time to RAFT's millisecond-based timing

## Current Status

✅ Library downloaded
✅ Header file created (`WillemtRaftApplication.h`)
⏳ Implementation in progress (`WillemtRaftApplication.cc`)
⏳ Makefile update needed
⏳ Testing needed

## Next Steps

1. Complete `WillemtRaftApplication.cc` implementation
2. Update Makefile to include raft.h
3. Test leader election
4. Integrate with intersection coordination logic

## Complexity Estimate

- Implementation: ~500-800 lines of code
- Testing: Need to verify leader election works correctly
- Integration: Should be straightforward once callbacks work

## Benefits Over Custom Implementation

- ✅ Proven algorithm (no bugs in core RAFT logic)
- ✅ Handles edge cases correctly
- ✅ Well-tested in production
- ✅ Proper tie-breaking and election timeouts
