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


namespace ns3 {

NS_LOG_COMPONENT_DEFINE("YtyCameraApplication");
NS_OBJECT_ENSURE_REGISTERED(YtyCamera);

// RtpHeader 类的实现代码
NS_OBJECT_ENSURE_REGISTERED(RtpHeader);
TypeId RtpHeader::GetTypeId(void) { static TypeId tid = TypeId("ns3::RtpHeader").SetParent<Header>().SetGroupName("Applications").AddConstructor<RtpHeader>(); return tid; }
RtpHeader::RtpHeader() : m_magic(0), m_timestamp(0), m_frameSeq(0), m_packetSeq(0), m_totalPackets(0) {}
RtpHeader::~RtpHeader() {}
TypeId RtpHeader::GetInstanceTypeId(void) const { return GetTypeId(); }
void RtpHeader::Print(std::ostream &os) const { os << "Magic=0x" << std::hex << (int)m_magic << std::dec << " Timestamp=" << m_timestamp << " FrameSeq=" << m_frameSeq; }
uint32_t RtpHeader::GetSerializedSize(void) const 
{ 
    return sizeof(m_magic) + sizeof(m_timestamp) + sizeof(m_frameSeq) + sizeof(m_packetSeq) + sizeof(m_totalPackets) + sizeof(m_packetsInFrame); 
}
void RtpHeader::Serialize(Buffer::Iterator start) const 
{ 
    start.WriteU8(m_magic);
    start.WriteHtonU64(m_timestamp); 
    start.WriteHtonU32(m_frameSeq); 
    start.WriteHtonU32(m_packetSeq); 
    start.WriteHtonU32(m_totalPackets); 
    start.WriteHtonU32(m_packetsInFrame); 
}
uint32_t RtpHeader::Deserialize(Buffer::Iterator start) 
{ 
    m_magic = start.ReadU8();
    m_timestamp = start.ReadNtohU64(); 
    m_frameSeq = start.ReadNtohU32(); 
    m_packetSeq = start.ReadNtohU32(); 
    m_totalPackets = start.ReadNtohU32(); 
    m_packetsInFrame = start.ReadNtohU32(); 
    return GetSerializedSize(); 
}


TypeId YtyCamera::GetTypeId(void)
{
    static TypeId tid = TypeId("ns3::YtyCamera")
        .SetParent<Application>()
        .SetGroupName("Applications")
        .AddConstructor<YtyCamera>()
        .AddAttribute("FrameRate", "The encoding frame rate in fps.", UintegerValue(30), MakeUintegerAccessor(&YtyCamera::m_frameRate), MakeUintegerChecker<uint32_t>())
        .AddAttribute("PacketSize", "The size of packets sent.", UintegerValue(1400), MakeUintegerAccessor(&YtyCamera::m_packetSize), MakeUintegerChecker<uint32_t>())
        .AddAttribute("RemoteAddress", "The destination address of the outbound packets", AddressValue(), MakeAddressAccessor(&YtyCamera::m_peerAddress), MakeAddressChecker())
        .AddAttribute("RemotePort", "The destination port of the outbound packets", UintegerValue(9), MakeUintegerAccessor(&YtyCamera::m_peerPort), MakeUintegerChecker<uint16_t>())
        .AddAttribute("CameraId", "此摄像头的唯一ID.", UintegerValue(0), MakeUintegerAccessor(&YtyCamera::m_cameraId), MakeUintegerChecker<uint32_t>())
        .AddAttribute("Codec", "The video codec (e.g., H.264, H.265).", StringValue("H.264"), MakeStringAccessor(&YtyCamera::m_codec), MakeStringChecker()); // <<< 【新增】Codec属性
    return tid;
}

YtyCamera::YtyCamera()
    : m_socket(0),
      m_frameRate(30),
      m_packetSize(1400),
      m_running(false),
      m_frameSeqCounter(0),
      m_cumulativePacketsSent(0),
      m_cameraId(0),
      m_sessionActive(false), // <<< 新增: 初始化会话状态为未激活

      // --- 【核心修改】初始化新的参数 ---
      m_codec("H.264"),
      m_resolution("640x480"), // 给一个初始的默认值
      m_crf(23),              // 给一个初始的默认值
      m_actualBitrate(500000) // 初始码率 500kbps

{
    NS_LOG_FUNCTION(this);
    // 在构造函数中创建 CodecSimulator 实例
    m_codecSimulator = std::make_unique<YtyCodecSimulator>(m_codec);
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

    
    // SendRtspRequest("PLAY");
    SendPlayRequestAndScheduleRetry();

    m_encoderEvent = Simulator::ScheduleNow(&YtyCamera::Encoder, this);
    m_sendEvent = Simulator::ScheduleNow(&YtyCamera::SendPacket, this);
}


void YtyCamera::StopApplication(void)
{
    NS_LOG_FUNCTION(this);
    m_running = false;

    SendRtspRequest("TEARDOWN");

    if (m_rtspRetryEvent.IsPending()) // <<< 新增
    {
        Simulator::Cancel(m_rtspRetryEvent);
    }

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

    
    // 如果帧率或码率为0，则不产生数据包
    if (m_actualBitrate == 0 || m_frameRate == 0) {
        // 安排下一次编码事件，以防码率后续恢复
        Time nextEncodeTime = Seconds(1.0 / m_frameRate);
        m_encoderEvent = Simulator::Schedule(nextEncodeTime, &YtyCamera::Encoder, this);
        return;
    }
    
    // 使用由 CodecSimulator 决定的真实码率
    uint32_t frameSize = m_actualBitrate / m_frameRate;
    uint32_t numPacketsInFrame = (frameSize / 8 + m_packetSize - 1) / m_packetSize;

    for (uint32_t i = 0; i < numPacketsInFrame; ++i)
    {
        Ptr<Packet> packet = Create<Packet>(m_packetSize);
        
        m_cumulativePacketsSent++;

        RtpHeader rtpHeader;
        // +++ 【核心修正】为每个RTP包设置魔数 +++
        rtpHeader.SetMagic(0xAC);
        
        rtpHeader.SetTimestamp(Simulator::Now().GetNanoSeconds());
        rtpHeader.SetFrameSeq(m_frameSeqCounter);
        rtpHeader.SetPacketSeq(i);
        rtpHeader.SetTotalPackets(m_cumulativePacketsSent);
        rtpHeader.SetPacketsInFrame(numPacketsInFrame); 
        
        packet->AddHeader(rtpHeader);
        m_sendBuffer.push(packet);
    }
    // 打印摄像头的编码信息
    // NS_LOG_INFO("At time " << Simulator::Now().GetSeconds() << "s, Camera " << m_cameraId << " encoded frame " << m_frameSeqCounter << " with " << numPacketsInFrame << " packets. (Bitrate: " << m_actualBitrate/1000 << "kbps, FPS: " << m_frameRate << ")");

    m_frameSeqCounter++;

    // 更新发送速率，以便 ScheduleTx 使用
    m_sendRate = DataRate(numPacketsInFrame * m_packetSize * 8 * m_frameRate);

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
        // 打印摄像头发送数据包的日志
        // NS_LOG_INFO("At time " << Simulator::Now().GetSeconds() << "s, Camera sent a packet of size " << packet->GetSize() << " bytes.");
    }

    ScheduleTx();
}

void YtyCamera::SendRtpPacket(Ptr<Packet> packet)
{
    m_socket->Send(packet);
}

void YtyCamera::SendRtspRequest(std::string method)
{
    std::ostringstream msg;
    if (method == "PLAY")
    {
        msg << "PLAY rtsp://server/video RTSP/1.0\r\n"
            << "CSeq: 1\r\n"
            << "X-Frame-Rate: " << m_frameRate << "\r\n"
            << "X-Camera-ID: " << m_cameraId << "\r\n\r\n"
            << "X-Codec: " << m_codec << "\r\n\r\n";
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


// +++ 【新增】上报新的编码参数给服务器 +++
void YtyCamera::SendEncodingParams()
{
    std::ostringstream msg;
    msg << "SET_PARAMS rtsp://server/video RTSP/1.0\r\n"
        << "CSeq: 2\r\n" // Use a different CSeq for this new request type
        << "X-Resolution: " << m_resolution << "\r\n"
        << "X-CRF: " << m_crf << "\r\n"
        << "X-Actual-Bitrate: " << m_actualBitrate << "\r\n\r\n";

    Ptr<Packet> packet = Create<Packet>((const uint8_t*)msg.str().c_str(), msg.str().length());
    m_socket->Send(packet);

    NS_LOG_INFO("At time " << Simulator::Now().GetSeconds() << "s, Camera " << m_cameraId << " sent SET_PARAMS to server.");
}


// +++ 【新增】根据服务器下发的带宽，更新编码参数 +++
void YtyCamera::UpdateEncodingParameters(uint32_t bandwidthBps)
{
    double target_kbps = bandwidthBps / 1000.0;
    EncodingParams params = m_codecSimulator->FindParams(target_kbps);

    if(params.found) {
        bool paramsChanged = (m_resolution != params.resolution || m_frameRate != (uint32_t)params.frame_rate || m_crf != (uint32_t)params.crf);

        // +++ 【核心修正 2】确保首次参数一定会被上报 +++
        // 上报条件：参数发生变化，或者会话尚未激活（意味着这是第一次计算参数）
        bool shouldSendUpdate = paramsChanged || !m_sessionActive;

        m_resolution = params.resolution;
        m_frameRate = params.frame_rate;
        m_crf = params.crf;
        m_actualBitrate = params.actual_bitrate_kbps * 1000; // 转换回 bps

        NS_LOG_INFO("At time " << Simulator::Now().GetSeconds() << "s, Camera " << m_cameraId << " updated params for bandwidth " << bandwidthBps/1000 << "kbps -> "
                    << "Res: " << m_resolution << ", FPS: " << m_frameRate << ", CRF: " << m_crf << ", Actual Bitrate: " << m_actualBitrate/1000 << "kbps");

        // 根据新的条件决定是否上报
        if (shouldSendUpdate) {
            SendEncodingParams();
            // 如果参数真的变化了（而非首次设置），才需要重新协商帧率
            if (paramsChanged) {
                 SendRtspRequest("PLAY");
            }
        }
    } else {
        NS_LOG_WARN("Camera " << m_cameraId << " could not find suitable encoding parameters for bandwidth " << target_kbps << "kbps.");
    }
}


void YtyCamera::HandleRead(Ptr<Socket> socket)
{
    NS_LOG_FUNCTION(this << socket);
    Ptr<Packet> packet;
    Address from;
    while ((packet = socket->RecvFrom(from)))
    {

        // 服务器的反馈包现在只包含一个uint32_t，即目标带宽
        if (packet->GetSize() == sizeof(uint32_t))
        {
            uint32_t receivedBandwidth = 0;
            packet->CopyData(reinterpret_cast<uint8_t*>(&receivedBandwidth), sizeof(uint32_t));
            
            NS_LOG_INFO("At time " << Simulator::Now().GetSeconds() << "s, Camera " << m_cameraId << " received available bandwidth from server: " << receivedBandwidth / 1000 << " kbps");
            
            // 根据收到的带宽，触发编码器参数更新流程
            UpdateEncodingParameters(receivedBandwidth);

            // +++ 【核心修正 1】如果这是第一条有效反馈，则激活会话并停止PLAY重试 +++
            if (!m_sessionActive)
            {
                NS_LOG_INFO("At time " << Simulator::Now().GetSeconds() << "s, Camera " << m_cameraId 
                            << " session is now active. Stopping PLAY retries.");
                m_sessionActive = true;
                if (m_rtspRetryEvent.IsPending())
                {
                    Simulator::Cancel(m_rtspRetryEvent);
                }
            }
        }
        else 
        {
            NS_LOG_WARN("收到一个未知类型的反馈包，大小为: " << packet->GetSize());
        }
    }
}


// 在 yty-camera.cc 文件中新增这个方法的实现
void YtyCamera::SendPlayRequestAndScheduleRetry()
{
    if (!m_running || m_sessionActive)
    {
        return; // 如果仿真停止或会话已激活，则停止重试
    }

    SendRtspRequest("PLAY"); // 发送PLAY请求
    
    // 安排1秒后再次尝试
    m_rtspRetryEvent = Simulator::Schedule(Seconds(1.0), &YtyCamera::SendPlayRequestAndScheduleRetry, this);
}

} // namespace ns3