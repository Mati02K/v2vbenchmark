#pragma once
#include <set>
#include <string>
#include <map>
#include "veins/modules/application/ieee80211p/DemoBaseApplLayer.h"
using namespace veins;

class HelloWaveApplication : public DemoBaseApplLayer
{
  public:
    void initialize(int stage) override;
    void finish() override;

  protected:
    void onWSM(BaseFrame1609_4* wsm) override;
    void handleSelfMsg(cMessage* msg) override;
    void handlePositionUpdate(cObject* obj) override;

  private:
    // ====== CONFIG ======
    static constexpr int TOTAL_VEHICLES = 4;
    // Base HELLO period and jitter
    const simtime_t basePeriod = SimTime(0.1);    // 100ms
    const simtime_t jitter     = SimTime(0.005);  // 5ms

    // Initial start jitter (first HELLO after first position update)
    const simtime_t initMin    = SimTime(0.05);   // 50ms
    const simtime_t initMax    = SimTime(0.10);   // 100ms

    // ACK backoff window (random delay before sending ACK to reduce collisions)
    const simtime_t ackBackoffMax = SimTime(0.05); // 50ms


    // ====== STATE ======
    int myId = -1;
    bool stopSendingHello = false;

    // Who has ACKed *my* HELLOs
    std::set<int> ackedSet;
    cMessage* helloEvent = nullptr;

    // ACK de-duplication: only ACK each sender once
    std::set<int> ackSentTo;

    // Delayed ACK timers: senderId -> timer
    std::map<int, cMessage*> ackTimers;

    // ====== BENCHMARKING ======
    int helloAttempts = 0;
    simtime_t startTime;
    simtime_t endTime;

  private:
    void scheduleHello(simtime_t delay);
    void sendHello();
    void sendAck(int targetId);
    void processHello(BaseFrame1609_4* wsm);
    void processAck(BaseFrame1609_4* wsm);
    int parseSenderId(const char* name) const;
    std::string setToString(const std::set<int>& s) const;
    std::string pendingAckToString() const;
};
