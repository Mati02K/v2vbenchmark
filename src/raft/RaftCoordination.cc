// RaftCoordination.cc — Follower-side coordination handlers
// When the leader sends requests, followers respond here.
// Also handles incoming vehicle-passed / vehicle-left notifications.

#include "raft/RaftAppBase.h"
#include "raft/RaftLogger.h"

#include <cstring>
#include <iostream>

#define NOW (simTime())

// ============ STATUS REQUEST / RESPONSE ============

void RaftAppBase::handleStatusRequest(int fromLeader)
{
    if (hasPassedIntersection_) return;
    // Only RAFT cluster members (lane leaders) respond to STATUS_REQUEST.
    // Queued vehicles are passive and wait for COORD_PASS_ORDER_BROADCAST instead.
    if (!raftServer_) {
        std::cout << simTime() << " [DBG][V" << myId_ << "] STATUS_REQUEST from V" << fromLeader
                  << " ignored — not a RAFT cluster member (queued vehicle)" << std::endl;
        return;
    }
    sendStatusResponse(fromLeader);
}

void RaftAppBase::sendStatusResponse(int toLeader)
{
    VehicleProposal proposal = buildMyProposal();
    proposal.isPriority = false;  // never self-claim priority — receiver sets this from cert

    // Build SignedProposal: serialize proposal, sign it, attach cert
    SignedProposal sp;
    memset(&sp, 0, sizeof(sp));

    sp.proposalSize = sizeof(VehicleProposal);
    memcpy(sp.proposalBytes, &proposal, sizeof(VehicleProposal));
    sp.timestampMs  = (uint64_t)(simTime().dbl() * 1000.0);
    sp.cert         = myCert_;

    CryptoAuth::instance().signProposal(myPrivKey_,
                                         sp.proposalBytes, sp.proposalSize,
                                         sp.timestampMs,
                                         sp.signature, sp.signatureLen);

    std::vector<uint8_t> data(sizeof(SignedProposal));
    memcpy(data.data(), &sp, sizeof(SignedProposal));
    sendRaftToPeer(toLeader, /*COORD_STATUS_RESPONSE*/ 0x31, data);
    messagesSent_++;
}

void RaftAppBase::handleStatusResponse(int fromVehicle, bool wos)
{
    if (!isLeader_ || !waitingForStatus_) return;
    collectedWayOfSight_[fromVehicle] = wos;
    statusResponseCount_++;
    if (statusResponseCount_ >= (int)activeVehicles_.size()) {
        collectStatusAndDecide();
    }
}

void RaftAppBase::handleStatusResponseProposal(int fromVehicle, const VehicleProposal& proposal)
{
    std::cout << simTime() << " [DBG][V" << myId_ << "] STATUS_RESPONSE from V" << fromVehicle
              << " isLeader=" << isLeader_ << " waitingForStatus=" << waitingForStatus_
              << " count=" << statusResponseCount_ << "/" << (totalVehicles_-1)
              << " wos=" << proposal.isFirstInLane
              << " dist=" << proposal.distanceToJunction << "m" << std::endl;
    if (!isLeader_ || !waitingForStatus_) return;
    collectedProposals_[fromVehicle] = proposal;
    collectedWayOfSight_[fromVehicle] = proposal.isFirstInLane;
    statusResponseCount_++;
    int expectedResponses = std::max(1, (int)activeVehicles_.size() - 1);
    std::cout << simTime() << " [DBG][V" << myId_ << "] STATUS collected " << statusResponseCount_
              << "/" << expectedResponses << " (need " << expectedResponses << ")" << std::endl;
    if (statusResponseCount_ >= expectedResponses) {
        waitingForStatus_ = false;
        if (timeStatusRequestSent_ > SIMTIME_ZERO) {
            statusCollectionTimeMs_ += (NOW - timeStatusRequestSent_).dbl() * 1000.0;
        }
        std::cout << simTime() << " [DBG][V" << myId_ << "] ALL STATUS COLLECTED -> proposePassOrder()" << std::endl;
        proposePassOrder();
    }
}

void RaftAppBase::collectStatusAndDecide()
{
    if (!isLeader_ || hasPassedIntersection_ || hasCommittedOrder_) return;
    waitingForStatus_ = false;
    if (timeStatusRequestSent_ > SIMTIME_ZERO) {
        statusCollectionTimeMs_ += (NOW - timeStatusRequestSent_).dbl() * 1000.0;
    }

    RAFT_LOG_LEADER("collected status from " << collectedWayOfSight_.size() << " vehicles");
    proposeStatusReport();
}

// ============ PASS ORDER BROADCAST (non-cluster vehicles) ============

// Non-cluster (queued) vehicles receive the committed schedule here.
// RAFT cluster members apply the schedule via doApplyLog() — they skip this.
void RaftAppBase::handlePassOrderBroadcast(const std::vector<uint8_t>& data)
{
    if (hasPassedIntersection_) return;
    if (hasCommittedOrder_) return;  // already have the schedule (duplicate broadcast)
    if (raftServer_) {
        // RAFT cluster member: schedule arrives via log replication, not broadcast.
        // Accept it here only as a fallback if log not yet applied.
        if (hasCommittedOrder_) return;
    }
    if (data.size() < sizeof(PassScheduleEntry)) {
        std::cout << simTime() << " [WARN][V" << myId_
                  << "] PASS_ORDER_BROADCAST: payload too small (" << data.size()
                  << " < " << sizeof(PassScheduleEntry) << ")" << std::endl;
        return;
    }

    PassScheduleEntry schedule;
    memcpy(&schedule, data.data(), sizeof(PassScheduleEntry));

    std::cout << simTime() << " [DBG][V" << myId_ << "] PASS_ORDER_BROADCAST received: "
              << schedule.numBatches << " batches (raftServer_=" << (raftServer_ != nullptr) << ")" << std::endl;

    memcpy(&committedSchedule_, &schedule, sizeof(PassScheduleEntry));
    hasCommittedOrder_ = true;
    coordinationMethod_ = "raft";

    if (hasStoppedAtIntersection_ && timeStopped_ > SIMTIME_ZERO)
        timeOrderCommitted_ = NOW;

    RAFT_LOG("PASS_ORDER_BROADCAST applied (" << committedSchedule_.numBatches << " batches)");
    applyCommittedPassOrder();
}

// ============ QUORUM CERTIFICATE ============
//
// After PASS_ORDER commits, the RAFT leader assembles a Quorum Certificate so that
// the next round's cluster can verify what was already scheduled without running RAFT
// again.  Flow:
//
//   Leader                     Follower(s)
//   ------                     -----------
//   sendQCSignRequest() ──────► handleQCSignRequest()
//                               sendQCSignResponse()  ──────► handleQCSignResponse()
//   tryAssembleQC()  (once majority reached)
//   sendQCBroadcast() ────────► handleQCBroadcast()  (all 16 vehicles store the QC)
//
// The signed data is: [uint32_t roundNumber_ || PassScheduleEntry].

void RaftAppBase::sendQCSignRequest()
{
    if (!isLeader_ || qcAssembled_) return;

    // Payload: [uint32_t round][PassScheduleEntry]
    size_t tbsSize = sizeof(uint32_t) + sizeof(PassScheduleEntry);
    std::vector<uint8_t> tbs(tbsSize);
    uint32_t rn = static_cast<uint32_t>(roundNumber_);
    memcpy(tbs.data(),                 &rn,                sizeof(uint32_t));
    memcpy(tbs.data() + sizeof(uint32_t), &committedSchedule_, sizeof(PassScheduleEntry));

    sendRaftBroadcast(/*QC_SIGN_REQUEST*/ 0x36, tbs);
    messagesSent_++;

    // Leader signs its own copy first.
    QCSigEntry mySig;
    memcpy(mySig.pubKey, myPubKey_, CRYPTO_PUBKEY_BYTES);
    mySig.sigLen = 0;
    CryptoAuth::instance().signBytes(myPrivKey_, tbs.data(), tbs.size(), mySig.sig, mySig.sigLen);
    collectedQCSigs_[myId_] = mySig;

    std::cout << NOW << " [QC][V" << myId_ << "] QC_SIGN_REQUEST broadcast (round="
              << roundNumber_ << " clusterSize=" << activeVehicles_.size() << ")" << std::endl;

    tryAssembleQC();  // check if already at quorum (single-node cluster edge case)
}

void RaftAppBase::handleQCSignRequest(const std::vector<uint8_t>& data, int senderId)
{
    // Only non-leader cluster members respond (leader signed its own copy already).
    if (isLeader_ || !raftServer_) return;
    if (hasPassedIntersection_) return;
    if (data.size() < sizeof(uint32_t) + sizeof(PassScheduleEntry)) return;

    // Sign the same TBS the leader used: [round || schedule]
    uint8_t sig[CRYPTO_SIG_MAX_BYTES];
    uint8_t sigLen = 0;
    CryptoAuth::instance().signBytes(myPrivKey_, data.data(), data.size(), sig, sigLen);

    // Response payload: [vehicleId:4B][pubKey:65B][sig:72B][sigLen:1B]
    std::vector<uint8_t> resp(sizeof(int) + CRYPTO_PUBKEY_BYTES + CRYPTO_SIG_MAX_BYTES + 1);
    size_t off = 0;
    memcpy(resp.data() + off, &myId_,    sizeof(int));            off += sizeof(int);
    memcpy(resp.data() + off, myPubKey_, CRYPTO_PUBKEY_BYTES);    off += CRYPTO_PUBKEY_BYTES;
    memcpy(resp.data() + off, sig,       CRYPTO_SIG_MAX_BYTES);   off += CRYPTO_SIG_MAX_BYTES;
    resp[off] = sigLen;

    sendQCSignResponse(senderId, resp);

    std::cout << NOW << " [QC][V" << myId_ << "] QC_SIGN_RESPONSE sent to leader V"
              << senderId << std::endl;
}

void RaftAppBase::sendQCSignResponse(int toLeader, const std::vector<uint8_t>& respData)
{
    sendRaftToPeer(toLeader, /*QC_SIGN_RESPONSE*/ 0x37, respData);
    messagesSent_++;
}

void RaftAppBase::handleQCSignResponse(const std::vector<uint8_t>& data, int senderId)
{
    if (!isLeader_ || qcAssembled_) return;
    if (data.size() < sizeof(int) + CRYPTO_PUBKEY_BYTES + CRYPTO_SIG_MAX_BYTES + 1) return;

    size_t off = 0;
    int vid;
    memcpy(&vid, data.data() + off, sizeof(int)); off += sizeof(int);

    if (vid != senderId) {
        std::cout << NOW << " [QC][V" << myId_ << "] QC_SIGN_RESPONSE: vehicleId mismatch"
                  << " (claimed " << vid << " from packet sender " << senderId << ") — ignored" << std::endl;
        return;
    }
    if (!activeVehicles_.count(vid)) return;  // not a cluster member

    QCSigEntry entry;
    memcpy(entry.pubKey, data.data() + off, CRYPTO_PUBKEY_BYTES); off += CRYPTO_PUBKEY_BYTES;
    memcpy(entry.sig,    data.data() + off, CRYPTO_SIG_MAX_BYTES); off += CRYPTO_SIG_MAX_BYTES;
    entry.sigLen = data[off];

    collectedQCSigs_[vid] = entry;

    int required = static_cast<int>(activeVehicles_.size()) / 2 + 1;
    std::cout << NOW << " [QC][V" << myId_ << "] QC_SIGN_RESPONSE from V" << vid
              << " (" << collectedQCSigs_.size() << "/" << required << " sigs)" << std::endl;

    tryAssembleQC();
}

void RaftAppBase::tryAssembleQC()
{
    if (!isLeader_ || qcAssembled_) return;
    int required = static_cast<int>(activeVehicles_.size()) / 2 + 1;
    if (static_cast<int>(collectedQCSigs_.size()) >= required)
        sendQCBroadcast();
}

void RaftAppBase::sendQCBroadcast()
{
    qcAssembled_ = true;

    QuorumCertificate qc;
    memset(&qc, 0, sizeof(qc));
    qc.valid    = true;
    qc.round    = static_cast<uint32_t>(roundNumber_);
    memcpy(&qc.schedule, &committedSchedule_, sizeof(PassScheduleEntry));
    qc.numSigs  = 0;

    for (auto& kv : collectedQCSigs_) {
        if (qc.numSigs >= QC_MAX_MEMBERS) break;
        QCSig& s     = qc.sigs[qc.numSigs];
        s.vehicleId  = kv.first;
        memcpy(s.pubKey, kv.second.pubKey, CRYPTO_PUBKEY_BYTES);
        memcpy(s.sig,    kv.second.sig,    CRYPTO_SIG_MAX_BYTES);
        s.sigLen     = kv.second.sigLen;
        qc.numSigs++;
    }

    // Store locally (leader is also a receiver of the next round).
    prevRoundQC_    = qc;
    hasPrevRoundQC_ = true;
    // Populate scheduledVehicles_ so our own next round skips these.
    for (int b = 0; b < qc.schedule.numBatches; b++)
        for (int v = 0; v < qc.schedule.batches[b].numVehicles; v++)
            scheduledVehicles_.insert(qc.schedule.batches[b].vehicleIds[v]);

    std::vector<uint8_t> data(sizeof(QuorumCertificate));
    memcpy(data.data(), &qc, sizeof(QuorumCertificate));
    sendRaftBroadcast(/*QC_BROADCAST*/ 0x38, data);
    messagesSent_++;

    std::cout << NOW << " [QC][V" << myId_ << "] QC assembled and broadcast: round="
              << qc.round << " numSigs=" << qc.numSigs
              << " scheduledVehicles_=" << scheduledVehicles_.size() << std::endl;
}

void RaftAppBase::handleQCBroadcast(const std::vector<uint8_t>& data)
{
    if (data.size() < sizeof(QuorumCertificate)) return;

    QuorumCertificate qc;
    memcpy(&qc, data.data(), sizeof(QuorumCertificate));

    if (!qc.valid || qc.numSigs == 0) return;

    // Optionally verify signatures — at minimum log validity.
    // Accept the QC if it is newer than what we already have.
    if (hasPrevRoundQC_ && qc.round <= prevRoundQC_.round) return;

    prevRoundQC_    = qc;
    hasPrevRoundQC_ = true;
    qcAssembled_    = true;  // suppress local QC assembly if we already have one

    // Populate scheduledVehicles_ so our next round skips these vehicles.
    for (int b = 0; b < qc.schedule.numBatches; b++) {
        for (int v = 0; v < qc.schedule.batches[b].numVehicles; v++) {
            scheduledVehicles_.insert(qc.schedule.batches[b].vehicleIds[v]);
        }
    }

    std::cout << NOW << " [QC][V" << myId_ << "] QC_BROADCAST received: round="
              << qc.round << " numSigs=" << qc.numSigs
              << " scheduledVehicles_ now=" << scheduledVehicles_.size() << std::endl;
}

bool RaftAppBase::verifyQC(const QuorumCertificate& qc) const
{
    if (!qc.valid || qc.numSigs == 0) return false;

    // Build the TBS buffer [uint32_t round || PassScheduleEntry] that signers signed.
    size_t tbsSize = sizeof(uint32_t) + sizeof(PassScheduleEntry);
    std::vector<uint8_t> tbs(tbsSize);
    memcpy(tbs.data(),                    &qc.round,    sizeof(uint32_t));
    memcpy(tbs.data() + sizeof(uint32_t), &qc.schedule, sizeof(PassScheduleEntry));

    int validSigs = 0;
    for (int i = 0; i < qc.numSigs; i++) {
        bool ok = CryptoAuth::instance().verifyBytes(
            qc.sigs[i].pubKey, tbs.data(), tbs.size(),
            qc.sigs[i].sig, qc.sigs[i].sigLen);
        if (ok) validSigs++;
        else
            std::cout << "[QC][V" << myId_ << "] verifyQC: sig " << i
                      << " from V" << qc.sigs[i].vehicleId << " INVALID" << std::endl;
    }
    return validSigs >= qc.numSigs;  // all claimed sigs must verify
}

// ============ VEHICLE PASSED / LEFT (follower-side) ============

void RaftAppBase::handleVehiclePassed(int vehicleId)
{
    std::cout << simTime() << " [DBG][V" << myId_ << "] VEHICLE_PASSED: V" << vehicleId
              << " frontVeh=" << vehicleInFrontOfMe_
              << " myBatch=" << myBatch_ << " curBatch=" << currentBatch_ << std::endl;
    RAFT_LOG("RECEIVED vehicle-passed from vehicle " << vehicleId);
    markRaftNodeInactive(vehicleId);
    activeVehicles_.erase(vehicleId);
    if (vehicleId == vehicleInFrontOfMe_) {
        wayOfSight_ = true;
        vehicleInFrontOfMe_ = -1;

        // Chain movement: when the vehicle in front of us passes, we must advance
        // toward the stop line. This applies BOTH to (a) vehicles not yet stopped,
        // AND (b) queued vehicles (hasStoppedAtIntersection_=true) waiting behind.
        if (!hasPassedIntersection_ && traciVehicle_ && timeStartedMoving_ == SIMTIME_ZERO) {
            std::cout << simTime() << " [CHAIN][V" << myId_ << "] Front V" << vehicleId
                      << " passed -> advancing to stop position (resume car-following)" << std::endl;
            RAFT_LOG("front vehicle passed — advancing to stop position (chain)");
            try {
                traciVehicle_->setSpeedMode(31);  // restore SUMO car-following
                traciVehicle_->setSpeed(-1);      // let SUMO advance us
            } catch (...) {}
        }
    }
}

void RaftAppBase::handleVehicleLeft(int vehicleId, int batchId)
{
    if (gossipSeenVehicleLeft_.count(vehicleId)) return;
    gossipSeenVehicleLeft_.insert(vehicleId);

    std::cout << simTime() << " [DBG][V" << myId_ << "] VEHICLE_LEFT: V" << vehicleId
              << " batch=" << batchId
              << " curBatch=" << currentBatch_
              << " vehiclesLeftSoFar=[";
    for (int v : vehiclesLeftInBatch_) std::cout << v << " ";
    std::cout << "]" << std::endl;
    RAFT_LOG("RECEIVED vehicle-left from vehicle " << vehicleId);

    markRaftNodeInactive(vehicleId);
    activeVehicles_.erase(vehicleId);
    vehiclesLeftInBatch_.insert(vehicleId);
    checkBatchAdvance();

    // Remove from vehicleDB_ after 2 seconds so lane leader flag stays fresh
    scheduleOneshotMs(2000.0, [this, vehicleId]() {
        vehicleDB_.erase(vehicleId);
        std::cout << simTime() << " [DBG][V" << myId_ << "] vehicleDB_ cleaned V"
                  << vehicleId << " (2s post-exit)" << std::endl;
    });
}
