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

    // +++ 用于识别RTP包的魔数 +++
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

    // 现在这个方法负责处理所有参数更新的复杂逻辑
    void UpdateEncodingParameters(uint32_t bandwidthBps);
    void SendEncodingParams();

    void ScheduleTx(void);
    void Encoder(void);
    void HandleRead(Ptr<Socket> socket);
    void SendRtpPacket(Ptr<Packet> packet);
    void SendRtspRequest(std::string method);



    Ptr<Socket> m_socket;
    Address m_peerAddress;
    uint16_t m_peerPort;

    // 帧率现在是动态可变的
    uint32_t m_frameRate;
    uint32_t m_packetSize;

    EventId m_encoderEvent;
    bool m_running;

    std::queue<Ptr<Packet>> m_sendBuffer;

    uint32_t m_frameSeqCounter;
    uint32_t m_cumulativePacketsSent;

    uint32_t m_cameraId; // 摄像头的唯一ID


    void SendPlayRequestAndScheduleRetry();
    bool m_sessionActive;      // 标记会话是否已激活
    EventId m_rtspRetryEvent;  // 用于RTSP PLAY重试的事件

    // Pacing 机制相关的成员变量
    EventId m_pacingEvent;         // 用于调度包间隔发送的事件ID
    Time    m_pacingInterval;      // 包与包之间的发送间隔

    // 核心参数变量
    std::unique_ptr<YtyCodecSimulator> m_codecSimulator; // 编码器模拟器实例
    std::string m_codec;        // 编码器类型 (H.264/H.265)
    std::string m_resolution;   // 当前分辨率
    uint32_t    m_crf;          // 当前CRF

    // 这个变量现在存储由 CodecSimulator 决定的【真实】码率
    uint32_t m_actualBitrate;

    // 用于智能分辨率切换的决策压力系统
    int32_t m_increaseResPressure;  // 提升分辨率的压力值
    int32_t m_decreaseResPressure;  // 降低分辨率的压力值
    const int32_t m_pressureThreshold; // 触发切换的压力阈值
    const int32_t m_pressureRecoveryRate; // 压力值的自然恢复速率

    bool m_initialParamsNegotiated;  // 用于标记是否已完成首次参数协商的标志

    // 应用程序最大码率限制 (0 表示不限制)
    DataRate m_maxAppBitrate;

    std::string m_videoComplexity;

};

} // namespace ns3

#endif /* YTY_CAMERA_H */