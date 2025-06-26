/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */

#include "yty-camera.h"
#include "ns3/log.h"
#include "ns3/ipv4-address.h"
#include "ns3/nstime.h"
#include "ns3/inet-socket-address.h"
#include "ns3/socket.h"
#include "ns3/simulator.h"
#include "ns3/socket-factory.h"
#include "ns3/packet.h"
#include "ns3/uinteger.h"
#include "ns3/boolean.h" // <<< 新增：包含布尔值头文件
#include "ns3/string.h"
#include <sstream>
#include "yty-bitrate-sampler.h"

namespace ns3 {

NS_LOG_COMPONENT_DEFINE("YtyCameraApplication");
NS_OBJECT_ENSURE_REGISTERED(YtyCamera);

// RtpHeader 类的实现代码
NS_OBJECT_ENSURE_REGISTERED(RtpHeader);
TypeId RtpHeader::GetTypeId(void) { static TypeId tid = TypeId("ns3::RtpHeader").SetParent<Header>().SetGroupName("Applications").AddConstructor<RtpHeader>(); return tid; }
RtpHeader::RtpHeader() : m_timestamp(0), m_frameSeq(0), m_packetSeq(0), m_totalPackets(0) {}
RtpHeader::~RtpHeader() {}
TypeId RtpHeader::GetInstanceTypeId(void) const { return GetTypeId(); }
void RtpHeader::Print(std::ostream &os) const { os << "Timestamp=" << m_timestamp << " FrameSeq=" << m_frameSeq << " PacketSeq=" << m_packetSeq << " TotalPackets=" << m_totalPackets << " PacketsInFrame=" << m_packetsInFrame; }
uint32_t RtpHeader::GetSerializedSize(void) const { return sizeof(m_timestamp) + sizeof(m_frameSeq) + sizeof(m_packetSeq) + sizeof(m_totalPackets) + sizeof(m_packetsInFrame); }
void RtpHeader::Serialize(Buffer::Iterator start) const { start.WriteHtonU64(m_timestamp); start.WriteHtonU32(m_frameSeq); start.WriteHtonU32(m_packetSeq); start.WriteHtonU32(m_totalPackets); start.WriteHtonU32(m_packetsInFrame); }
uint32_t RtpHeader::Deserialize(Buffer::Iterator start) { m_timestamp = start.ReadNtohU64(); m_frameSeq = start.ReadNtohU32(); m_packetSeq = start.ReadNtohU32(); m_totalPackets = start.ReadNtohU32(); m_packetsInFrame = start.ReadNtohU32(); return GetSerializedSize(); }


TypeId YtyCamera::GetTypeId(void)
{
    static TypeId tid = TypeId("ns3::YtyCamera")
        .SetParent<Application>()
        .SetGroupName("Applications")
        .AddConstructor<YtyCamera>()
        // <<< 移除：Bitrate属性，因为它现在是动态的
        // .AddAttribute("Bitrate", "The encoding bitrate in bps.", UintegerValue(1000000), MakeUintegerAccessor(&YtyCamera::m_bitrate), MakeUintegerChecker<uint32_t>())
        .AddAttribute("FrameRate", "The encoding frame rate in fps.", UintegerValue(30), MakeUintegerAccessor(&YtyCamera::m_frameRate), MakeUintegerChecker<uint32_t>())
        .AddAttribute("PacketSize", "The size of packets sent.", UintegerValue(1400), MakeUintegerAccessor(&YtyCamera::m_packetSize), MakeUintegerChecker<uint32_t>())
        .AddAttribute("RemoteAddress", "The destination address of the outbound packets", AddressValue(), MakeAddressAccessor(&YtyCamera::m_peerAddress), MakeAddressChecker())
        .AddAttribute("RemotePort", "The destination port of the outbound packets", UintegerValue(9), MakeUintegerAccessor(&YtyCamera::m_peerPort), MakeUintegerChecker<uint16_t>())
        .AddAttribute("LogFile", "File to log statistics.", StringValue("scratch/camera_stats.txt"), MakeStringAccessor(&YtyCamera::m_logFileName), MakeStringChecker())
        // ▼▼▼ 新增属性 ▼▼▼
        .AddAttribute("EnableLog", "Enable or disable logging.", BooleanValue(true), MakeBooleanAccessor(&YtyCamera::m_logEnabled), MakeBooleanChecker());
    return tid;
}

YtyCamera::YtyCamera()
    : m_socket(0),
    //   m_bitrate(1000000),
      m_frameRate(30),
      m_packetSize(1400),
      m_running(false),
      m_frameSeqCounter(0),
      m_cumulativePacketsSent(0),
      m_throughput(0.0),
      m_delay(Seconds(0.0)),
      m_lossRate(0.0),
      m_logFileName("camera_stats.txt"),
      m_logEnabled(false), // <<< 新增：默认启用日志
      m_bitrateSampler(nullptr) // <<< 新增：初始化采样器指针
{
    NS_LOG_FUNCTION(this);
}

YtyCamera::~YtyCamera()
{
    NS_LOG_FUNCTION(this);
    m_socket = 0;
}

void YtyCamera::SetRemote(Address ip, uint16_t port)
{
    NS_LOG_FUNCTION(this << ip << port);
    m_peerAddress = ip;
    m_peerPort = port;
}

// <<< 新增：实现设置采样器的方法
void YtyCamera::SetBitrateSampler(Ptr<BitrateSampler> sampler)
{
    NS_LOG_FUNCTION(this << sampler);
    m_bitrateSampler = sampler;
}

void YtyCamera::DoDispose(void)
{
    NS_LOG_FUNCTION(this);
    Application::DoDispose();
}

void YtyCamera::StartApplication(void)
{
    NS_LOG_FUNCTION(this);
    m_running = true;

    // ▼▼▼ 修改部分：检查日志开关 ▼▼▼
    if (m_logEnabled)
    {
        m_logFile.open(m_logFileName, std::ios::out | std::ios::trunc);
        if (m_logFile.is_open())
        {
            m_logFile << "Timestamp(s)\tThroughput(bps)\tDelay(ms)\tLossRate" << std::endl;
        }
    }

    if (!m_socket)
    {
        TypeId tid = TypeId::LookupByName("ns3::UdpSocketFactory");
        m_socket = Socket::CreateSocket(GetNode(), tid);
        if (m_socket->Bind() == -1)
        {
            NS_FATAL_ERROR("Failed to bind socket");
        }
        m_socket->Connect(InetSocketAddress(Ipv4Address::ConvertFrom(m_peerAddress), m_peerPort));
    }
    m_socket->SetRecvCallback(MakeCallback(&YtyCamera::HandleRead, this));

    // 码率采样所做的修改
    // uint32_t frameSizeInBits = m_bitrate / m_frameRate;
    // uint32_t numPacketsInFrame = (frameSizeInBits / 8 + m_packetSize - 1) / m_packetSize;
    // uint32_t actualBitrate = numPacketsInFrame * m_packetSize * 8 * m_frameRate;
    // m_sendRate = DataRate(actualBitrate);
    
    SendRtspRequest("PLAY");

    m_encoderEvent = Simulator::ScheduleNow(&YtyCamera::Encoder, this);
    m_sendEvent = Simulator::ScheduleNow(&YtyCamera::SendPacket, this);
}


void YtyCamera::StopApplication(void)
{
    NS_LOG_FUNCTION(this);
    m_running = false;

    // ▼▼▼ 修改部分：检查日志开关 ▼▼▼
    if (m_logEnabled && m_logFile.is_open())
    {
        m_logFile.close();
    }

    SendRtspRequest("TEARDOWN");

    if (m_sendEvent.IsPending())
    {
        Simulator::Cancel(m_sendEvent);
    }
    if (m_encoderEvent.IsPending())
    {
        Simulator::Cancel(m_encoderEvent);
    }
    if (m_socket)
    {
        m_socket->Close();
    }
}

void YtyCamera::Encoder(void)
{
    NS_LOG_FUNCTION(this);
    if (!m_running) return;

    // uint32_t frameSize = m_bitrate / m_frameRate;

    // <<< 关键修改：现在，该函数在编码每一帧视频之前，都会通过 m_bitrateSampler->Sample() 方法获取一个新的、动态的码率值。 >>>
    uint32_t currentBitrate = 0;
    if (m_bitrateSampler)
    {
        currentBitrate = m_bitrateSampler->Sample();
    } else {
        NS_LOG_WARN("Bitrate sampler not set for camera node " << GetNode()->GetId() << ". Using 0 bps.");
    }
    NS_LOG_INFO("Node " << GetNode()->GetId() << " sampled new bitrate: " << currentBitrate << " bps");

    // 根据新码率计算帧大小和包数量
    uint32_t frameSize = currentBitrate / m_frameRate;


    uint32_t numPacketsInFrame = (frameSize / 8 + m_packetSize - 1) / m_packetSize;

    for (uint32_t i = 0; i < numPacketsInFrame; ++i)
    {
        Ptr<Packet> packet = Create<Packet>(m_packetSize);
        
        m_cumulativePacketsSent++;

        RtpHeader rtpHeader;
        rtpHeader.SetTimestamp(Simulator::Now().GetNanoSeconds());
        rtpHeader.SetFrameSeq(m_frameSeqCounter);
        rtpHeader.SetPacketSeq(i);
        rtpHeader.SetTotalPackets(m_cumulativePacketsSent);
        rtpHeader.SetPacketsInFrame(numPacketsInFrame); 
        
        packet->AddHeader(rtpHeader);
        m_sendBuffer.push(packet);
    }
    NS_LOG_INFO("At time " << Simulator::Now().GetSeconds() << "s, Camera encoded frame " << m_frameSeqCounter << " with " << numPacketsInFrame << " packets.");

    m_frameSeqCounter++;

    // 更新发送速率，以便 ScheduleTx 使用
    uint32_t actualBitrate = numPacketsInFrame * m_packetSize * 8 * m_frameRate;
    m_sendRate = DataRate(actualBitrate);

    // 安排下一次编码事件
    Time nextEncodeTime = Seconds(1.0 / m_frameRate);
    m_encoderEvent = Simulator::Schedule(nextEncodeTime, &YtyCamera::Encoder, this);
}


void YtyCamera::ScheduleTx(void)
{
    if (m_running)
    {

        if (m_sendRate == DataRate(0)) {
            // 如果速率为0（比如码率采样为0），则不需要频繁调度发送
            // 可以在Encoder中重新启动它
        return;
        }

        Time txInterval = m_sendRate.CalculateBytesTxTime(m_packetSize);
        m_sendEvent = Simulator::Schedule(txInterval, &YtyCamera::SendPacket, this);
    }
}

void YtyCamera::SendPacket(void)
{
    NS_LOG_FUNCTION(this);
    if (!m_running) return;

    if (!m_sendBuffer.empty())
    {
        Ptr<Packet> packet = m_sendBuffer.front();
        m_sendBuffer.pop();
        SendRtpPacket(packet);
        NS_LOG_INFO("At time " << Simulator::Now().GetSeconds() << "s, Camera sent a packet of size " << packet->GetSize() << " bytes.");
    }

    ScheduleTx();
}

void YtyCamera::SendRtpPacket(Ptr<Packet> packet)
{
    m_socket->Send(packet);
    PathDecision();
}

void YtyCamera::SendRtspRequest(std::string method)
{
    std::ostringstream msg;
    if (method == "PLAY")
    {
        msg << "PLAY rtsp://server/video RTSP/1.0\r\n"
            << "CSeq: 1\r\n"
            << "X-Frame-Rate: " << m_frameRate << "\r\n\r\n";
    }
    else
    {
        msg << method << " rtsp://server/video RTSP/1.0\r\n"
            << "CSeq: 1\r\n\r\n";
    }
    
    Ptr<Packet> packet = Create<Packet>((const uint8_t*)msg.str().c_str(), msg.str().length());
    m_socket->Send(packet);

    NS_LOG_INFO("At time " << Simulator::Now().GetSeconds() << "s, Camera sent RTSP " << method << " request.");
}

void YtyCamera::HandleRead(Ptr<Socket> socket)
{
    NS_LOG_FUNCTION(this << socket);
    Ptr<Packet> packet;
    Address from;
    while ((packet = socket->RecvFrom(from)))
    {
        uint32_t expectedSize = sizeof(double) + sizeof(int64_t) + sizeof(double);
        if (packet->GetSize() >= expectedSize)
        {
            uint8_t* buffer = new uint8_t[expectedSize];
            packet->CopyData(buffer, expectedSize);
            
            uint32_t offset = 0;

            m_throughput = *(reinterpret_cast<double*>(buffer + offset));
            offset += sizeof(double);

            int64_t delay_ns = *(reinterpret_cast<int64_t*>(buffer + offset));
            m_delay = NanoSeconds(delay_ns);
            offset += sizeof(int64_t);

            m_lossRate = *(reinterpret_cast<double*>(buffer + offset));
            
            delete[] buffer;

            NS_LOG_INFO("At time " << Simulator::Now().GetSeconds() 
                        << "s, Camera received RTCP feedback: Throughput=" << m_throughput 
                        << " bps, Delay=" << m_delay.GetMilliSeconds() 
                        << " ms, Loss Rate=" << m_lossRate);

            WriteStatsToFile();
            PathDecision();
        }
    }
}

void YtyCamera::PathDecision(void)
{
    // 此处是实现路径选择的地方，现在写实现拥塞控制和码率自适应逻辑的地方
}

void YtyCamera::WriteStatsToFile()
{
    // ▼▼▼ 修改部分：检查日志开关 ▼▼▼
    if (m_logEnabled && m_logFile.is_open())
    {
        m_logFile << Simulator::Now().GetSeconds() << "\t"
                  << m_throughput << "\t"
                  << m_delay.GetMilliSeconds() << "\t"
                  << m_lossRate << std::endl;
    }
}

} // namespace ns3