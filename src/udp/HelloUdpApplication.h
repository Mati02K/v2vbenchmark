#pragma once
#include <set>
#include <string>
#include "veins_inet/VeinsInetApplicationBase.h"

class HelloUdpApplication : public veins::VeinsInetApplicationBase
{
  public:
    HelloUdpApplication();
    virtual ~HelloUdpApplication();

  protected:
    virtual bool startApplication() override;
    virtual bool stopApplication() override;
    virtual void processPacket(std::shared_ptr<inet::Packet> pk) override;

  private:
    // ====== CONFIG ======
    static constexpr int TOTAL_VEHICLES = 4;
    const simtime_t basePeriod = SimTime(0.1);   // 100ms
    const simtime_t jitter     = SimTime(0.005);  // 5ms
    const simtime_t initMin    = SimTime(0.05);  // 50ms
    const simtime_t initMax    = SimTime(0.10);  // 100ms

    // ====== STATE ======
    int myId = -1;
    bool stopSendingHello = false;
    std::set<int> ackedSet;  // WHO has ACKed my HELLO messages (this is what matters!)

    long helloHandle = -1;

    // ====== BENCHMARKING ======
    int helloAttempts = 0;  // Count how many HELLO messages sent
    simtime_t startTime;    // When did we start
    simtime_t endTime;      // When did we complete

  private:
    void scheduleHello(simtime_t delay);
    void sendHello();
    void sendAck(int targetId);
    void processHello(const std::string& packetName);
    void processAck(const std::string& packetName);

    int parseSenderId(const char* name) const;
    std::string setToString(const std::set<int>& s) const;
    std::string pendingAckToString() const;
};
