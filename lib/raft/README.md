# RAFT Library Integration

This directory contains the RAFT consensus library.

## Library: willemt/raft

- **Repository**: https://github.com/willemt/raft
- **Type**: Single-header library (raft.h)
- **License**: BSD 2-Clause
- **Features**: Leader election, log replication

## Installation

Download `raft.h` from the repository and place it in this directory.

```bash
cd third_party/raft
wget https://raw.githubusercontent.com/willemt/raft/master/include/raft.h
```

## Integration Notes

The library provides:
- `raft_node` - represents a node in the cluster
- `raft_server` - the main RAFT server instance
- Callbacks for sending messages, receiving messages, applying log entries

We need to:
1. Create a transport layer that bridges RAFT callbacks to OMNeT++ UDP messages
2. Map RAFT node IDs to vehicle IDs
3. Handle RAFT timeouts using OMNeT++ timers
