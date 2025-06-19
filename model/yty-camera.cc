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
#include "ns3/string.h"
#include <sstream>

namespace ns3 {

NS_LOG_COMPONENT_DEFINE("YtyCameraApplication");
NS_OBJECT_ENSURE_REGISTERED(YtyCamera);

// RtpHeader 类的实现代码
NS_OBJECT_ENSURE_REGISTERED(RtpHeader);
TypeId RtpHeader::GetTypeId(void) { static TypeId tid = TypeId("ns3::RtpHeader").SetParent<Header>().SetGroupName("Applications").AddConstructor<RtpHeader>(); return tid; }
RtpHeader::RtpHeader() : m_timestamp(0), m_frameSeq(0), m_packetSeq(0), m_totalPackets(0) {}
RtpHeader::~RtpHeader() {}
TypeId RtpHeader::GetInstanceTypeId(void) const { return GetTypeId(); }
void RtpHeader::Print(std::ostream &os) const { os << "Timestamp=" << m_timestamp << " FrameSeq=" << m_frameSeq << " PacketSeq=" << m_packetSeq << " TotalPackets=" << m_totalPackets; }
uint32_t RtpHeader::GetSerializedSize(void) const { return sizeof(m_timestamp) + sizeof(m_frameSeq) + sizeof(m_packetSeq) + sizeof(m_totalPackets); }
void RtpHeader::Serialize(Buffer::Iterator start) const { start.WriteHtonU64(m_timestamp); start.WriteHtonU32(m_frameSeq); start.WriteHtonU32(m_packetSeq); start.WriteHtonU32(m_totalPackets); }
uint32_t RtpHeader::Deserialize(Buffer::Iterator start) { m_timestamp = start.ReadNtohU64(); m_frameSeq = start.ReadNtohU32(); m_packetSeq = start.ReadNtohU32(); m_totalPackets = start.ReadNtohU32(); return GetSerializedSize(); }


TypeId YtyCamera::GetTypeId(void)
{
    static TypeId tid = TypeId("ns3::YtyCamera")
        .SetParent<Application>()
        .SetGroupName("Applications")
        .AddConstructor<YtyCamera>()
        .AddAttribute("Bitrate", "The encoding bitrate in bps.", UintegerValue(1000000), MakeUintegerAccessor(&YtyCamera::m_bitrate), MakeUintegerChecker<uint32_t>())
        .AddAttribute("FrameRate", "The encoding frame rate in fps.", UintegerValue(30), MakeUintegerAccessor(&YtyCamera::m_frameRate), MakeUintegerChecker<uint32_t>())
        .AddAttribute("PacketSize", "The size of packets sent.", UintegerValue(1400), MakeUintegerAccessor(&YtyCamera::m_packetSize), MakeUintegerChecker<uint32_t>())
        .AddAttribute("RemoteAddress", "The destination address of the outbound packets", AddressValue(), MakeAddressAccessor(&YtyCamera::m_peerAddress), MakeAddressChecker())
        .AddAttribute("RemotePort", "The destination port of the outbound packets", UintegerValue(9), MakeUintegerAccessor(&YtyCamera::m_peerPort), MakeUintegerChecker<uint16_t>())
        // ▼▼▼ 修改部分 1：添加LogFile属性 ▼▼▼
        .AddAttribute("LogFile", "File to log statistics.", StringValue("camera_stats.txt"), MakeStringAccessor(&YtyCamera::m_logFileName), MakeStringChecker());
    return tid;
}

YtyCamera::YtyCamera()
    : m_socket(0),
      m_bitrate(1000000),
      m_frameRate(30),
      m_packetSize(1400),
      m_running(false),
      m_frameSeqCounter(0),
      m_throughput(0.0),
      m_delay(Seconds(0.0)),
      m_lossRate(0.0),
      // ▼▼▼ 修改部分 2：在构造函数中初始化m_logFileName ▼▼▼
      m_logFileName("camera_stats.txt")
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

void YtyCamera::DoDispose(void)
{
    NS_LOG_FUNCTION(this);
    Application::DoDispose();
}

void YtyCamera::StartApplication(void)
{
    NS_LOG_FUNCTION(this);
    m_running = true;

    // ▼▼▼ 修改部分 3：打开文件并写入表头 ▼▼▼
    m_logFile.open(m_logFileName, std::ios::out | std::ios::trunc);
    if (m_logFile.is_open())
    {
        m_logFile << "Timestamp(s)\tThroughput(bps)\tDelay(ms)\tLossRate" << std::endl;
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

    m_sendRate = DataRate(m_bitrate);

    SendRtspRequest("PLAY");

    m_encoderEvent = Simulator::ScheduleNow(&YtyCamera::Encoder, this);
    m_sendEvent = Simulator::ScheduleNow(&YtyCamera::SendPacket, this);
}

void YtyCamera::StopApplication(void)
{
    NS_LOG_FUNCTION(this);
    m_running = false;

    // ▼▼▼ 修改部分 4：关闭文件流 ▼▼▼
    if (m_logFile.is_open())
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

    uint32_t frameSize = m_bitrate / m_frameRate;
    uint32_t numPackets = (frameSize / 8 + m_packetSize - 1) / m_packetSize;

    for (uint32_t i = 0; i < numPackets; ++i)
    {
        Ptr<Packet> packet = Create<Packet>(m_packetSize);
        
        RtpHeader rtpHeader;
        rtpHeader.SetTimestamp(Simulator::Now().GetNanoSeconds());
        rtpHeader.SetFrameSeq(m_frameSeqCounter);
        rtpHeader.SetPacketSeq(i);
        rtpHeader.SetTotalPackets(numPackets);
        
        packet->AddHeader(rtpHeader);
        m_sendBuffer.push(packet);
    }
    NS_LOG_INFO("At time " << Simulator::Now().GetSeconds() << "s, Camera encoded frame " << m_frameSeqCounter << " with " << numPackets << " packets.");

    m_frameSeqCounter++;

    Time nextEncodeTime = Seconds(1.0 / m_frameRate);
    m_encoderEvent = Simulator::Schedule(nextEncodeTime, &YtyCamera::Encoder, this);
}

void YtyCamera::ScheduleTx(void)
{
    if (m_running)
    {
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
    msg << method << " rtsp://server/video RTSP/1.0\r\n"
        << "CSeq: 1\r\n\r\n";
    
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
        if (packet->GetSize() > 0)
        {
            uint8_t* buffer = new uint8_t[packet->GetSize()];
            packet->CopyData(buffer, packet->GetSize());
            
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

            // ▼▼▼ 修改部分 5：调用写文件函数 ▼▼▼
            WriteStatsToFile();
            PathDecision();
        }
    }
}

void YtyCamera::PathDecision(void)
{
    // This function is still a placeholder for future logic
}

// ▼▼▼ 修改部分 6：添加写文件函数的完整实现 ▼▼▼
void YtyCamera::WriteStatsToFile()
{
    if (m_logFile.is_open())
    {
        m_logFile << Simulator::Now().GetSeconds() << "\t"
                  << m_throughput << "\t"
                  << m_delay.GetMilliSeconds() << "\t"
                  << m_lossRate << std::endl;
    }
}

} // namespace ns3