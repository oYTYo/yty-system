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
#include "yty-codec-simulator.h"


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

    // +++ 【新增】用于识别RTP包的魔数 +++
    void SetMagic(uint8_t magic) { m_magic = magic; }
    uint8_t GetMagic() const { return m_magic; }

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
    uint8_t  m_magic;       // 魔数，例如 0xAC
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


protected:
    virtual void DoDispose(void);

private:
    virtual void StartApplication(void);
    virtual void StopApplication(void);

    // +++ 【新增】新的私有方法，用于处理码率决策和参数上报 +++
    void UpdateEncodingParameters(uint32_t bandwidthBps);
    void SendEncodingParams();

    void ScheduleTx(void);
    void SendPacket(void);
    void Encoder(void);
    void HandleRead(Ptr<Socket> socket);
    void SendRtpPacket(Ptr<Packet> packet);
    void SendRtspRequest(std::string method);


    Ptr<Socket> m_socket;
    Address m_peerAddress;
    uint16_t m_peerPort;

    uint32_t m_frameRate;
    uint32_t m_packetSize;
    DataRate m_sendRate;

    EventId m_sendEvent;
    EventId m_encoderEvent;
    bool m_running;

    std::queue<Ptr<Packet>> m_sendBuffer;

    uint32_t m_frameSeqCounter;
    uint32_t m_cumulativePacketsSent;

    uint32_t m_cameraId; // 摄像头的唯一ID


    void SendPlayRequestAndScheduleRetry(); // 新增一个方法声明
    bool m_sessionActive;      // <<< 新增: 标记会话是否已激活
    EventId m_rtspRetryEvent;  // <<< 新增: 用于RTSP PLAY重试的事件

    // --- 【核心修改】用我们新的编码器模拟器和参数变量替代旧的逻辑 ---
    std::unique_ptr<YtyCodecSimulator> m_codecSimulator; // 编码器模拟器实例
    std::string m_codec;        // 编码器类型 (H.264/H.265)
    std::string m_resolution;   // 当前分辨率
    uint32_t    m_crf;          // 当前CRF

    // 这个变量现在存储由 CodecSimulator 决定的【真实】码率
    uint32_t m_actualBitrate;


};

} // namespace ns3

#endif /* YTY_CAMERA_H */