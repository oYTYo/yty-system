/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */

#ifndef YTY_CAMERA_H
#define YTY_CAMERA_H

#include "ns3/application.h"
#include "ns3/event-id.h"
#include "ns3/ptr.h"
#include "ns3/address.h"
#include "ns3/traced-callback.h"
#include "ns3/data-rate.h" // <<< FIX: Added missing header for DataRate

#include <queue>
#include <fstream>

namespace ns3 {

class Socket;
class Packet;

// ▼▼▼ FIX: Converted RtpHeader from a struct to a proper ns3::Header class ▼▼▼
class RtpHeader : public Header
{
public:
    static TypeId GetTypeId(void);
    RtpHeader();
    virtual ~RtpHeader();

    // Required virtual functions from ns3::Header
    virtual TypeId GetInstanceTypeId(void) const;
    virtual void Print(std::ostream &os) const;
    virtual uint32_t GetSerializedSize(void) const;
    virtual void Serialize(Buffer::Iterator start) const;
    virtual uint32_t Deserialize(Buffer::Iterator start);

    // Setters and Getters for our data
    void SetTimestamp(uint64_t ts) { m_timestamp = ts; }
    uint64_t GetTimestamp() const { return m_timestamp; }

    void SetFrameSeq(uint32_t seq) { m_frameSeq = seq; }
    uint32_t GetFrameSeq() const { return m_frameSeq; }
    
    void SetPacketSeq(uint32_t seq) { m_packetSeq = seq; }
    uint32_t GetPacketSeq() const { return m_packetSeq; }
    
    void SetTotalPackets(uint32_t count) { m_totalPackets = count; }
    uint32_t GetTotalPackets() const { return m_totalPackets; }


private:
    uint64_t m_timestamp;   // Changed to 64-bit for nanosecond precision
    uint32_t m_frameSeq;
    uint32_t m_packetSeq;
    uint32_t m_totalPackets;
};
// ▲▲▲ End of RtpHeader class fix ▲▲▲


/**
 * @brief A mock camera application
 */
class YtyCamera : public Application
{
public:
    static TypeId GetTypeId(void);
    YtyCamera();
    virtual ~YtyCamera();

    void SetRemote(Address ip, uint16_t port);

protected:
    virtual void DoDispose(void);

private:
    virtual void StartApplication(void);
    virtual void StopApplication(void);

    void ScheduleTx(void);
    void SendPacket(void);
    void Encoder(void);
    void HandleRead(Ptr<Socket> socket);
    void SendRtpPacket(Ptr<Packet> packet);
    void SendRtspRequest(std::string method);
    void PathDecision(void);
    void WriteStatsToFile();

    Ptr<Socket> m_socket;
    Address m_peerAddress;
    uint16_t m_peerPort;

    uint32_t m_bitrate;
    uint32_t m_frameRate;
    uint32_t m_packetSize;
    DataRate m_sendRate; // This now compiles correctly

    EventId m_sendEvent;
    EventId m_encoderEvent;
    bool m_running;

    std::queue<Ptr<Packet>> m_sendBuffer;

    uint32_t m_frameSeqCounter;
    
    double m_throughput;
    Time m_delay;
    double m_lossRate;

    std::string m_logFileName;
    std::ofstream m_logFile;
};

} // namespace ns3

#endif /* YTY_CAMERA_H */

// 我测试一下git