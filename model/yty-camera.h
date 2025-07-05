/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */

#ifndef YTY_CAMERA_H
#define YTY_CAMERA_H

#include "ns3/application.h"
#include "ns3/event-id.h"
#include "ns3/ptr.h"
#include "ns3/address.h"
#include "ns3/traced-callback.h"
#include "ns3/data-rate.h"

#include <queue>
#include <fstream>

#include "ns3/data-rate.h"

namespace ns3 {

/**
 * @brief 前向声明 BitrateSampler 类
 * 这可以在编译期间，当完整的类定义还未被解析时，提前告知编译器该类的存在，
 * 以解决循环依赖和编译顺序问题。
 */
class BitrateSampler;

class Socket;
class Packet;


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

    void SetPacketsInFrame(uint32_t count) { m_packetsInFrame = count; }
    uint32_t GetPacketsInFrame() const { return m_packetsInFrame; }


private:
    uint64_t m_timestamp;
    uint32_t m_frameSeq;
    uint32_t m_packetSeq;
    uint32_t m_totalPackets;
    uint32_t m_packetsInFrame;
};



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

    /**
    * @brief 设置码率采样器
    * @param sampler 指向码率采样器实例的智能指针
    */
    void SetBitrateSampler(Ptr<BitrateSampler> sampler); // <<< 新增：设置采样器的方法

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

    // uint32_t m_bitrate;
    uint32_t m_frameRate;
    uint32_t m_packetSize;
    DataRate m_sendRate;

    EventId m_sendEvent;
    EventId m_encoderEvent;
    bool m_running;

    std::queue<Ptr<Packet>> m_sendBuffer;

    uint32_t m_frameSeqCounter;
    uint32_t m_cumulativePacketsSent;
    
    double m_throughput;
    Time m_delay;
    double m_lossRate;

    std::string m_logFileName;
    std::ofstream m_logFile;
    bool m_logEnabled; // <<< 新增：日志启用/禁用开关

    Ptr<BitrateSampler> m_bitrateSampler; // <<< 新增了一个指向 BitrateSampler 对象的智能指针 Ptr<BitrateSampler> m_bitrateSampler，让每个摄像头实例都可以持有一个采样器。

    uint32_t m_cameraId; // 摄像头的唯一ID


    void SendPlayRequestAndScheduleRetry(); // 新增一个方法声明
    bool m_sessionActive;      // <<< 新增: 标记会话是否已激活
    EventId m_rtspRetryEvent;  // <<< 新增: 用于RTSP PLAY重试的事件

    // double m_decayFactor; // <<<【移除】不再需要衰减因子
    uint32_t m_targetBitrate; // <<<【新增】用于存储从服务器获取的目标码率 (bps)
    
    std::string m_codec; // <<< 【新增】用于存储摄像头编码格式的成员变量

};

} // namespace ns3

#endif /* YTY_CAMERA_H */