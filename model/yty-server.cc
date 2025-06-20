/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */

#include "yty-server.h"
#include "ns3/log.h"
#include "ns3/ipv4-address.h"
#include "ns3/nstime.h"
#include "ns3/inet-socket-address.h"
#include "ns3/socket.h"
#include "ns3/simulator.h"
#include "ns3/socket-factory.h"
#include "ns3/packet.h"
#include "ns3/uinteger.h"
#include "ns3/double.h"

namespace ns3 {

NS_LOG_COMPONENT_DEFINE("YtyServerApplication");
NS_OBJECT_ENSURE_REGISTERED(YtyServer);

TypeId YtyServer::GetTypeId(void)
{
    static TypeId tid = TypeId("ns3::YtyServer")
        .SetParent<Application>()
        .SetGroupName("Applications")
        .AddConstructor<YtyServer>()
        .AddAttribute("Port", "Port on which we listen for incoming packets.",
                      UintegerValue(9),
                      MakeUintegerAccessor(&YtyServer::m_port),
                      MakeUintegerChecker<uint16_t>())
        .AddAttribute("ReportInterval", "Interval for sending RTCP reports in milliseconds.",
                      TimeValue(MilliSeconds(50)),
                      MakeTimeAccessor(&YtyServer::m_reportInterval),
                      MakeTimeChecker());
    return tid;
}

YtyServer::YtyServer()
    : m_socket(0),
      m_port(9)
{
    NS_LOG_FUNCTION(this);
}

YtyServer::~YtyServer()
{
    NS_LOG_FUNCTION(this);
    m_socket = 0;
}

void YtyServer::DoDispose(void)
{
    NS_LOG_FUNCTION(this);
    Application::DoDispose();
}

void YtyServer::StartApplication(void)
{
    NS_LOG_FUNCTION(this);

    if (!m_socket)
    {
        TypeId tid = TypeId::LookupByName("ns3::UdpSocketFactory");
        m_socket = Socket::CreateSocket(GetNode(), tid);
        InetSocketAddress local = InetSocketAddress(Ipv4Address::GetAny(), m_port);
        if (m_socket->Bind(local) == -1)
        {
            NS_FATAL_ERROR("Failed to bind socket");
        }
    }

    m_socket->SetRecvCallback(MakeCallback(&YtyServer::HandleRead, this));
}

void YtyServer::StopApplication(void)
{
    NS_LOG_FUNCTION(this);
    for (auto const& [addr, session] : m_sessions) {
        // FIX: Changed IsRunning() to IsPending()
        if(session.reportEvent.IsPending()) Simulator::Cancel(session.reportEvent);
        if(session.playEvent.IsPending()) Simulator::Cancel(session.playEvent);
    }
    m_sessions.clear();

    if (m_socket)
    {
        m_socket->Close();
    }
}

void YtyServer::HandleRead(Ptr<Socket> socket)
{
    NS_LOG_FUNCTION(this << socket);
    Ptr<Packet> packet;
    Address from;
    while ((packet = socket->RecvFrom(from)))
    {
        // 复制少量起始数据用于安全地检查包类型
        uint8_t buffer[16];
        packet->CopyData(buffer, std::min((uint32_t)16, packet->GetSize()));
        std::string start(reinterpret_cast<char*>(buffer), std::min((uint32_t)16, packet->GetSize()));

        // 通过检查关键字来判断是否为RTSP请求包
        if (start.rfind("PLAY", 0) == 0 || start.rfind("TEARDOWN", 0) == 0)
        {
             ProcessRtsp(packet, from);
        }
        else // 否则，我们假设它是一个RTP包
        {
             ProcessRtp(packet, from);
        }
    }
}


void YtyServer::ProcessRtp(Ptr<Packet> packet, const Address& from)
{
    if (m_sessions.find(from) == m_sessions.end())
    {
        NS_LOG_WARN("Received RTP from unknown client " << InetSocketAddress::ConvertFrom(from).GetIpv4());
        return;
    }
    
    ClientSession& session = m_sessions[from];

    // --- ▼▼▼ 【已修改】更新统计逻辑 ▼▼▼ ---
    uint32_t packetSize = packet->GetSize(); // 获取实际包大小

    RtpHeader rtpHeader;
    packet->RemoveHeader(rtpHeader);

    Time sentTime = NanoSeconds(rtpHeader.GetTimestamp());
    Time now = Simulator::Now();
    Time delay = now - sentTime;
    
    // 累加当前周期的统计值
    session.currentTotalReceivedPackets++;
    session.currentTotalReceivedBytes += packetSize;
    session.currentTotalDelay += delay;
    
    // 更新我们所看到的、对方已发送的最大包数
    // 注意：这里假设GetTotalPackets()是累计值，如果不是，需要修改摄像头端
    // 为了简单起见，我们直接使用帧序号和包序号来估算
    uint32_t estimatedSentCount = rtpHeader.GetFrameSeq() * 10 + rtpHeader.GetPacketSeq(); // 这是一个简化的估算
    if (estimatedSentCount > session.maxSeenSentPackets) {
        session.maxSeenSentPackets = estimatedSentCount;
    }
    // --- ▲▲▲ 修改结束 ▲▲▲ ---

    session.recvBuffer[rtpHeader.GetFrameSeq()].push_back(packet);

    NS_LOG_INFO("At time " << now.GetSeconds() << "s, Server received packet " << rtpHeader.GetPacketSeq() 
                << " for frame " << rtpHeader.GetFrameSeq() << " from " << InetSocketAddress::ConvertFrom(from).GetIpv4()
                << ". Delay: " << delay.GetMilliSeconds() << " ms.");
}

void YtyServer::ProcessRtsp(Ptr<Packet> packet, const Address& from)
{
    uint8_t buffer[100];
    packet->CopyData(buffer, packet->GetSize());
    // 确保字符串以空字符结尾
    if (packet->GetSize() < 100)
    {
        buffer[packet->GetSize()] = '\0';
    } else {
        buffer[99] = '\0';
    }
    
    std::string request(reinterpret_cast<char*>(buffer));

    if (request.find("PLAY") != std::string::npos)
    {
        NS_LOG_INFO("At time " << Simulator::Now().GetSeconds() << "s, Server received PLAY request from " << InetSocketAddress::ConvertFrom(from).GetIpv4());
        if (m_sessions.find(from) == m_sessions.end())
        {
            m_sessions[from] = ClientSession();
        }
        ScheduleReport(from);
        m_sessions[from].playEvent = Simulator::Schedule(Seconds(1.0 / m_sessions[from].playFrameRate), &YtyServer::Playback, this, from);
    }
    else if (request.find("TEARDOWN") != std::string::npos)
    {
        NS_LOG_INFO("At time " << Simulator::Now().GetSeconds() << "s, Server received TEARDOWN request from " << InetSocketAddress::ConvertFrom(from).GetIpv4());
        if (m_sessions.count(from)) {
            // 使用 IsPending() 检查事件是否正在等待执行
            if (m_sessions[from].reportEvent.IsPending())
            {
                Simulator::Cancel(m_sessions[from].reportEvent);
            }
            if (m_sessions[from].playEvent.IsPending())
            {
                Simulator::Cancel(m_sessions[from].playEvent);
            }
            m_sessions.erase(from);
        }
    }
}


void YtyServer::ScheduleReport(const Address& clientAddress)
{
    if (m_sessions.count(clientAddress)) {
        m_sessions[clientAddress].reportEvent = Simulator::Schedule(m_reportInterval, &YtyServer::SendRtcpFeedback, this, clientAddress);
    }
}

void YtyServer::SendRtcpFeedback(const Address& clientAddress)
{
    if (!m_sessions.count(clientAddress)) return;

    ClientSession& session = m_sessions[clientAddress];
    Time now = Simulator::Now();

    // --- ▼▼▼ 【核心修改】基于时间窗口的差值计算 ▼▼▼ ---

    // 1. 计算自上次报告以来的时间差
    Time interval = now - session.lastReportTime;
    if (interval.IsZero())
    {
        // 避免除以零，如果间隔为0则跳过此次报告
        ScheduleReport(clientAddress);
        return;
    }

    // 2. 计算此时间窗口内接收到的包数和字节数
    uint64_t intervalPackets = session.currentTotalReceivedPackets - session.lastTotalReceivedPackets;
    uint64_t intervalBytes = session.currentTotalReceivedBytes - session.lastTotalReceivedBytes;

    // 3. 计算此时间窗口内的平均吞吐量
    double throughput = (intervalBytes * 8) / interval.GetSeconds();

    // 4. 计算此时间窗口内的平均时延
    Time intervalDelay = session.currentTotalDelay - session.lastTotalDelay;
    Time avgDelay = (intervalPackets > 0) ? intervalDelay / intervalPackets : Seconds(0);

    // 5. 计算此时间窗口内的丢包率
    // (这是一个简化的估算，更精确的方法需要在RTP头中加入累计已发送包总数)
    uint32_t intervalSent = session.maxSeenSentPackets - session.lastReportedSentPackets;
    double lossRate = 0.0;
    if (intervalSent > 0)
    {
        // 确保接收数不超过发送数
        uint64_t received = std::min((uint64_t)intervalSent, intervalPackets);
        lossRate = 1.0 - (double)received / intervalSent;
    }
    // 防止因估算不准出现负数
    if (lossRate < 0) lossRate = 0.0;

    // --- ▲▲▲ 计算结束 ▲▲▲ ---

    // 使用计算出的瞬时值来创建RTCP包
    uint32_t payloadSize = sizeof(double) + sizeof(int64_t) + sizeof(double);
    uint8_t* buffer = new uint8_t[payloadSize];
    uint32_t offset = 0;

    memcpy(buffer + offset, &throughput, sizeof(double));
    offset += sizeof(double);
    int64_t delay_ns = avgDelay.GetNanoSeconds();
    memcpy(buffer + offset, &delay_ns, sizeof(int64_t));
    offset += sizeof(int64_t);
    memcpy(buffer + offset, &lossRate, sizeof(double));
    
    Ptr<Packet> rtcpPacket = Create<Packet>(buffer, payloadSize);
    delete[] buffer;

    m_socket->SendTo(rtcpPacket, 0, clientAddress);

    NS_LOG_INFO("At time " << now.GetSeconds() << "s, Server sent RTCP to " << InetSocketAddress::ConvertFrom(clientAddress).GetIpv4()
                << ": IntervalThroughput=" << throughput << " bps, IntervalAvgDelay=" << avgDelay.GetMilliSeconds() << " ms, IntervalLossRate=" << lossRate);

    // --- ▼▼▼ 【重要】更新上一周期的状态，为下一次计算做准备 ▼▼▼ ---
    session.lastTotalReceivedPackets = session.currentTotalReceivedPackets;
    session.lastTotalReceivedBytes = session.currentTotalReceivedBytes;
    session.lastTotalDelay = session.currentTotalDelay;
    session.lastReportedSentPackets = session.maxSeenSentPackets;
    session.lastReportTime = now;
    // --- ▲▲▲ 更新结束 ▲▲▲ ---

    ScheduleReport(clientAddress);
}

void YtyServer::Playback(const Address& clientAddress)
{
    if (!m_sessions.count(clientAddress)) return;

    ClientSession& session = m_sessions[clientAddress];

    if (session.recvBuffer.count(session.playFrameSeq))
    {
        session.recvBuffer.erase(session.playFrameSeq);
        NS_LOG_INFO("At time " << Simulator::Now().GetSeconds() << "s, Server played frame " << session.playFrameSeq << " from client " << InetSocketAddress::ConvertFrom(clientAddress).GetIpv4());
        session.playFrameSeq++;
    }
    else
    {
        NS_LOG_INFO("At time " << Simulator::Now().GetSeconds() << "s, Frame " << session.playFrameSeq << " not available for playback for client " << InetSocketAddress::ConvertFrom(clientAddress).GetIpv4());
    }
    
    Time nextPlayTime = Seconds(1.0 / session.playFrameRate);
    session.playEvent = Simulator::Schedule(nextPlayTime, &YtyServer::Playback, this, clientAddress);
}

} // namespace ns3