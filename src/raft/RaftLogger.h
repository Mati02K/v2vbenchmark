#pragma once
// RaftLogger.h — Structured logging macros for RAFT intersection applications
//
// Replaces 80+ repeated occurrences of:
//   std::cout << std::fixed << std::setprecision(1)
//             << (simTime().dbl() * 1000.0) << "ms Vehicle " << myId_
//             << " <message>" << std::endl;
//
// Usage inside any class that has members simTime() and myId_:
//   RAFT_LOG("FORMING CLUSTER with " << n << " members");
//   RAFT_LOG_LEADER("submitted PASS_SCHEDULE entry #" << idx);
//   RAFT_LOG_WARN("No PASS_ORDER after " << ms << "ms");
//   RAFT_LOG_ERR("Could not create RAFT server!");

#include <iostream>
#include <iomanip>

// Per-vehicle timestamped log line
#define RAFT_LOG(msg) \
    std::cout << std::fixed << std::setprecision(1) \
              << (simTime().dbl() * 1000.0) << "ms Vehicle " << myId_ \
              << " " << msg << "\n"

// Same but labels as "Leader" – visual distinction in logs
#define RAFT_LOG_LEADER(msg) \
    std::cout << std::fixed << std::setprecision(1) \
              << (simTime().dbl() * 1000.0) << "ms Leader " << myId_ \
              << " " << msg << "\n"

// Warning — still to stdout but prefixed
#define RAFT_LOG_WARN(msg) \
    std::cout << std::fixed << std::setprecision(1) \
              << (simTime().dbl() * 1000.0) << "ms [WARN] Vehicle " << myId_ \
              << " " << msg << "\n"

// OMNeT++ EV_ERROR channel
#define RAFT_LOG_ERR(msg) \
    EV_ERROR << "Vehicle " << myId_ << " " << msg << endl
