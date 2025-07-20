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
#include "ns3/random-variable-stream.h" 


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
        // 帧率属性现在只是一个初始值，后面会动态改变
        .AddAttribute("FrameRate", "The initial encoding frame rate in fps.", UintegerValue(30), MakeUintegerAccessor(&YtyCamera::m_frameRate), MakeUintegerChecker<uint32_t>())
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
      m_sessionActive(false), // 初始化会话状态为未激活

      // 将压力系统相关的常量和变量初始化
      m_increaseResPressure(0),
      m_decreaseResPressure(0),
      m_pressureThreshold(100), // 设定一个阈值，例如100
      m_pressureRecoveryRate(10), // 设定一个恢复速率，例如每次降低10
      m_initialParamsNegotiated(false)

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

    // 在启动应用时，根据Codec类型完成最终的初始化
    m_codecSimulator = std::make_unique<YtyCodecSimulator>(m_codec);

    m_resolution = "N/A";
    m_frameRate = 30; // 保留一个默认帧率用于计算
    m_crf = 0;
    m_actualBitrate = 0; // 启动时码率为0，不发送数据

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

    
    SendPlayRequestAndScheduleRetry();

}


void YtyCamera::StopApplication(void)
{
    NS_LOG_FUNCTION(this);
    m_running = false;

    SendRtspRequest("TEARDOWN");

    if (m_rtspRetryEvent.IsPending())
    {
        Simulator::Cancel(m_rtspRetryEvent);
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
    uint32_t frameSizeBytes = m_actualBitrate / (8 * m_frameRate);
    uint32_t numPacketsInFrame = (frameSizeBytes + m_packetSize - 1) / m_packetSize;

    uint32_t bytesSentInFrame = 0;
    for (uint32_t i = 0; i < numPacketsInFrame; ++i)
    {
        // 计算当前这个包应该有的大小
        uint32_t packetPayloadSize = std::min((uint32_t)m_packetSize, frameSizeBytes - bytesSentInFrame);
        if (packetPayloadSize == 0) continue; // 防止产生0字节的包

        Ptr<Packet> packet = Create<Packet>(packetPayloadSize);
        bytesSentInFrame += packetPayloadSize;
        
        m_cumulativePacketsSent++;

        RtpHeader rtpHeader;
        rtpHeader.SetMagic(0xAC);
        rtpHeader.SetTimestamp(Simulator::Now().GetNanoSeconds());
        rtpHeader.SetFrameSeq(m_frameSeqCounter);
        rtpHeader.SetPacketSeq(i);
        rtpHeader.SetTotalPackets(m_cumulativePacketsSent);
        rtpHeader.SetPacketsInFrame(numPacketsInFrame); 
        
        packet->AddHeader(rtpHeader);
        m_sendBuffer.push(packet);
    }
    m_frameSeqCounter++;

    ScheduleTx(); 

    // 安排下一次编码事件
    Time nextEncodeTime = Seconds(1.0 / m_frameRate);
    m_encoderEvent = Simulator::Schedule(nextEncodeTime, &YtyCamera::Encoder, this);
}


// 它不再是调度单个包，而是循环发送，直到缓冲区为空。
void YtyCamera::ScheduleTx(void)
{
    if (m_running && !m_sendBuffer.empty())
    {
        // 从缓冲区取出一个包并发送
        Ptr<Packet> packet = m_sendBuffer.front();
        m_sendBuffer.pop();
        m_socket->Send(packet);

        // 只要缓冲区不为空，就立即安排下一次发送（在仿真时间上是“立刻”）
        Simulator::ScheduleNow(&YtyCamera::ScheduleTx, this);
    }
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


void YtyCamera::UpdateEncodingParameters(uint32_t bandwidthBps)
{
    double target_kbps = bandwidthBps / 1000.0;
    int switch_res_direction = 0; // 0=不切换, 1=升, -1=降

    // 1. 更新决策压力值
    // 先在当前分辨率下探测一下，如果按当前带宽，CRF会是多少
    EncodingParams probe_params = m_codecSimulator->FindBestParams(target_kbps, m_resolution, m_frameRate, 0);

    if (probe_params.found) {
        switch(probe_params.qualityLevel) {
            case CRF_QUALITY_TOO_HIGH:
                // CRF过低，画质太好，增加“升分辨率”的压力
                // CRF越低，压力增加越快
                m_increaseResPressure += (21 - probe_params.crf) * 10;
                // 同时缓慢恢复“降分辨率”的压力
                m_decreaseResPressure = std::max(0, m_decreaseResPressure - m_pressureRecoveryRate);
                break;
            case CRF_QUALITY_TOO_LOW:
                // CRF过高，画质太差，增加“降分辨率”的压力
                // CRF越高，压力增加越快
                m_decreaseResPressure += (probe_params.crf - 28) * 10;
                 // 同时缓慢恢复“升分辨率”的压力
                m_increaseResPressure = std::max(0, m_increaseResPressure - m_pressureRecoveryRate);
                break;
            case CRF_QUALITY_GOOD:
                // CRF在舒适区，双向压力都缓慢恢复
                m_increaseResPressure = std::max(0, m_increaseResPressure - m_pressureRecoveryRate);
                m_decreaseResPressure = std::max(0, m_decreaseResPressure - m_pressureRecoveryRate);
                break;
        }
    }

    // 2. 检查压力是否达到阈值，决定是否切换分辨率
    if (m_increaseResPressure >= m_pressureThreshold) {
        switch_res_direction = 1; // 触发升档
        m_increaseResPressure = 0; // 清空压力
    } else if (m_decreaseResPressure >= m_pressureThreshold) {
        switch_res_direction = -1; // 触发降档
        m_decreaseResPressure = 0; // 清空压力
    }
    
    // 3. 调用真正的参数查找函数
    EncodingParams final_params = m_codecSimulator->FindBestParams(target_kbps, m_resolution, m_frameRate, switch_res_direction);

    if(final_params.found) {
        // --- [核心逻辑检查] ---
        // 检查是否有任何参数发生了变化。
        bool anyParamsChanged = (m_resolution != final_params.resolution ||
                                 m_frameRate != (uint32_t)final_params.frame_rate ||
                                 m_crf != (uint32_t)final_params.crf);

        // 更新摄像头的内部状态。
        m_resolution = final_params.resolution;
        m_frameRate = final_params.frame_rate;
        m_crf = final_params.crf;
        m_actualBitrate = final_params.actual_bitrate_kbps * 1000; // 转换回 bps

        // 只要有任何参数变化，就通过 SET_PARAMS 通知服务器记录日志。
        // 【重要】不再发送 PLAY 请求进行重协商。
        if (anyParamsChanged) {
            SendEncodingParams();
        }
    } else {
        NS_LOG_WARN("Camera " << m_cameraId << " 这个带宽下没有合适的视频参数 " << target_kbps << "kbps.");
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

            // 增加一个小的随机延迟，防止所有摄像头同时决策造成拥塞风暴
            double random_delay = CreateObject<UniformRandomVariable>()->GetValue(0.01, 0.05);
            Simulator::Schedule(Seconds(random_delay), &YtyCamera::UpdateEncodingParameters, this, receivedBandwidth);

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

            // 只有在首次协商未完成时，才执行特殊逻辑
            if (!m_initialParamsNegotiated)
            {
                // 这是第一次收到带宽反馈
                // 1. 更新参数 (上面的 Schedule 已经安排了)

                // 2. 标记协商已完成
                m_initialParamsNegotiated = true;

                // 3. 启动编码器和发送事件
                m_encoderEvent = Simulator::ScheduleNow(&YtyCamera::Encoder, this);
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