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

#include "ns3/string.h"
#include <fstream>
#include <string>
#include <cmath> // 包含 cmath 以使用 std::abs


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
        .AddAttribute("ReportInterval", "Interval for sending RTCP reports.",
                      TimeValue(MilliSeconds(50)),
                      MakeTimeAccessor(&YtyServer::m_reportInterval),
                      MakeTimeChecker())
        // 为日志记录添加新属性
        .AddAttribute("LogFile", "File to log playback statistics.",
                      StringValue("scratch/play_status.txt"),
                      MakeStringAccessor(&YtyServer::m_logFileName),
                      MakeStringChecker())
        .AddAttribute("LogInterval", "Interval for logging playback stats.",
                      TimeValue(Seconds(1.0)),
                      MakeTimeAccessor(&YtyServer::m_logInterval),
                      MakeTimeChecker());
    return tid;
}

YtyServer::YtyServer() : m_socket(0), m_port(9) {}
YtyServer::~YtyServer() { m_socket = 0; }

void YtyServer::DoDispose(void)
{
    Application::DoDispose();
}


// VVV 新增: 实现客户端信息注册方法 VVV
void YtyServer::RegisterClientInfo(const Ipv4Address& clientIp, const ClientInfo& info)
{
    NS_LOG_FUNCTION(this << clientIp << info.accessType << info.region);
    m_clientInfoRegistry[clientIp] = info;
    NS_LOG_INFO("Registered: IP=" << clientIp 
                << ", CamID=" << info.cameraId 
                << ", Type=" << info.accessType
                << ", Region=" << info.region);
}
// ^^^ 新增 ^^^


void YtyServer::StartApplication(void)
{
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

    // ▼▼▼ 【修改】在日志表头中加入 "Jitter(ms)" ▼▼▼
    m_logFile.open(m_logFileName, std::ios::out | std::ios::trunc);
    if (m_logFile.is_open())
    {
        m_logFile << "Time(s)\tClientAddr\tThroughput(bps)\tDelay(ms)\tLossRate\tJitter(ms)\tPlayedFrames\tStutterEvents\tStutterRate\tCameraId\tAccessType\tRegion" << std::endl;
    }
    // ▲▲▲ 【修改】在日志表头中加入 "Jitter(ms)" ▲▲▲
}

void YtyServer::StopApplication(void)
{
    for (auto const& [addr, session] : m_sessions) {
        if(session.reportEvent.IsPending()) {
            Simulator::Cancel(session.reportEvent);
            Simulator::Cancel(session.playbackEvent);
            Simulator::Cancel(session.stutterTimeoutEvent);
            Simulator::Cancel(session.logStatsEvent);
        }
    }
    m_sessions.clear();

    if (m_logFile.is_open())
    {
        m_logFile.close();
    }

    if (m_socket)
    {
        m_socket->Close();
    }

}

void YtyServer::HandleRead(Ptr<Socket> socket)
{
    Ptr<Packet> packet;
    Address from;
    while ((packet = socket->RecvFrom(from)))
    {
        // 简单地通过包大小来区分RTP和RTSP (这是一种简化，但在本场景下有效)
        if (packet->GetSize() > 100) // Assume larger packets are RTP
        {
             ProcessRtp(packet, from);
        }
        else // Assume smaller packets are RTSP
        {
             ProcessRtsp(packet, from);
        }
    }
}


void YtyServer::ProcessRtp(Ptr<Packet> packet, const Address& from)
{
    // 如果会话还没有通过 PLAY 请求启动，则忽略数据包
    if (m_sessions.find(from) == m_sessions.end())
    {
        return;
    }
    
    ClientSession& session = m_sessions[from];
    uint32_t packetSize = packet->GetSize();
    Time now = Simulator::Now();

    // 创建一个副本用于读取头，因为 RemoveHeader 会修改原始包
    Ptr<Packet> packetCopy = packet->Copy();
    RtpHeader rtpHeader;
    packetCopy->RemoveHeader(rtpHeader);

    Time sentTime = NanoSeconds(rtpHeader.GetTimestamp());
    Time delay = now - sentTime;
    
    // 更新网络统计
    session.intervalReceivedPackets++;
    session.intervalReceivedBytes += packetSize;
    session.intervalTotalDelay += delay;
    uint32_t cumulativeSentCount = rtpHeader.GetTotalPackets();
    if (cumulativeSentCount > session.maxSeenSentPackets) {
        session.maxSeenSentPackets = cumulativeSentCount;
    }

    // --- VVV 新增：抖动计算逻辑 (基于 RFC 3550) VVV ---
    if (!session.lastArrivalTime.IsZero()) {
        Time transit = now - sentTime;
        Time lastTransit = session.lastArrivalTime - session.lastSentTime;
        
        int64_t diff_ns = std::abs(transit.GetNanoSeconds() - lastTransit.GetNanoSeconds());
        double diff_s = diff_ns / 1e9; // 转换为秒

        // 使用平滑算法更新抖动: J = J + (|D| - J) / 16
        session.jitter += (diff_s - session.jitter) / 16.0;
    }
    session.lastSentTime = sentTime;
    session.lastArrivalTime = now;
    // --- ^^^ 新增 ^^^ ---

    // --- 新的抖动缓冲逻辑 ---
    uint32_t frameSeq = rtpHeader.GetFrameSeq();
    uint32_t packetSeq = rtpHeader.GetPacketSeq();

    // 将数据包存入抖动缓冲区
    session.buffer[frameSeq][packetSeq] = {packet, now};
    // 打印服务器缓冲区日志
    // NS_LOG_INFO("At time " << now.GetSeconds() << "s, Server buffered packet for frame " << frameSeq << ", packet " << packetSeq);

    // 如果这就是我们当前正在等待的帧，立即尝试播放它
    if(frameSeq == session.nextFrameToPlay)
    {
        TryPlayback(from);
    }
}

void YtyServer::ProcessRtsp(Ptr<Packet> packet, const Address& from)
{
    uint8_t buffer[100];
    packet->CopyData(buffer, packet->GetSize());
    buffer[std::min((uint32_t)99, packet->GetSize())] = '\0';
    std::string request(reinterpret_cast<char*>(buffer));

    if (request.rfind("PLAY", 0) == 0)
    {   
        Ipv4Address clientIp = InetSocketAddress::ConvertFrom(from).GetIpv4();
        NS_LOG_INFO("At time " << Simulator::Now().GetSeconds() << "s, Server received PLAY request from " << clientIp);
        if (m_sessions.find(from) == m_sessions.end())
        {
            // m_sessions[from] = ClientSession();
            // m_sessions[from].lastReportTime = Simulator::Now();

            // VVV 修改: 核心逻辑 - 从注册表查找信息 VVV
            auto it = m_clientInfoRegistry.find(clientIp);
            if (it == m_clientInfoRegistry.end())
            {
                NS_LOG_WARN("Server received PLAY from an unregistered IP: " << clientIp << ". Ignoring.");
                return;
            }
            m_sessions[from] = ClientSession();
            ClientSession& session = m_sessions[from];
            session.lastReportTime = Simulator::Now();
            // 将预先注册的信息填充到当前会话中
            session.clientInfo = it->second; 
            // ^^^ 修改 ^^^

            // --- 新增：解析帧率 ---
            std::string header_key = "X-Frame-Rate: ";
            size_t pos = request.find(header_key);
            if (pos != std::string::npos)
            {
                // 提取帧率字符串并转换为整数
                std::string rate_str = request.substr(pos + header_key.length());
                try {
                    uint32_t negotiatedRate = std::stoul(rate_str);
                    m_sessions[from].frameRate = negotiatedRate;
                    NS_LOG_INFO("Negotiated frame rate with " << InetSocketAddress::ConvertFrom(from).GetIpv4() << ": " << negotiatedRate << " fps");
                } catch (const std::exception& e) {
                    NS_LOG_WARN("Failed to parse frame rate from request. Using default: " << m_sessions[from].frameRate);
                }
            }
            else
            {
                NS_LOG_INFO("No frame rate header found. Using default: " << m_sessions[from].frameRate);
            }
            // --- 解析结束 ---

            // --- 启动播放和日志记录 ---
            SchedulePlayback(from);
            ScheduleLog(from);
        }
        ScheduleReport(from);
    }
    else if (request.rfind("TEARDOWN", 0) == 0)
    {
        NS_LOG_INFO("At time " << Simulator::Now().GetSeconds() << "s, Server received TEARDOWN request from " << InetSocketAddress::ConvertFrom(from).GetIpv4());
        if (m_sessions.count(from)) {
            if (m_sessions[from].reportEvent.IsPending())
            {
                Simulator::Cancel(m_sessions[from].reportEvent);
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

    Time interval = now - session.lastReportTime;
    if (interval.IsZero())
    {
        ScheduleReport(clientAddress);
        return;
    }

    double throughput = (session.intervalReceivedBytes * 8) / interval.GetSeconds();
    Time avgDelay = (session.intervalReceivedPackets > 0) ? session.intervalTotalDelay / session.intervalReceivedPackets : Seconds(0);

    uint32_t intervalSent = session.maxSeenSentPackets - session.lastReportedSentPackets;
    double lossRate = 0.0;
    if (intervalSent > 0)
    {
        uint64_t receivedInInterval = std::min((uint64_t)intervalSent, session.intervalReceivedPackets);
        lossRate = 1.0 - (double)receivedInInterval / intervalSent;
    }
    if (lossRate < 0) lossRate = 0.0;

    // Create and send RTCP packet
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

    // ▼▼▼ 【新增】为日志记录累加抖动值 ▼▼▼
    session.logIntervalSumThroughput += throughput;
    session.logIntervalSumDelay += avgDelay;
    session.logIntervalSumLossRate += lossRate;
    session.logIntervalSumJitter += session.jitter; // 累加当前计算的抖动值
    session.logIntervalRtcpCount++;
    // ▲▲▲ 【新增】为日志记录累加抖动值 ▲▲▲


    // ▼▼▼ 添加调试日志 ▼▼▼
    // NS_LOG_INFO("--- DEBUG --- "
    //             << "Time: " << now.GetSeconds() << "s, "
    //             << "Client: " << InetSocketAddress::ConvertFrom(clientAddress).GetIpv4() << ", "
    //             << "TotalDelay before reset: " << session.intervalTotalDelay.GetMilliSeconds() << "ms, "
    //             << "Packets in interval: " << session.intervalReceivedPackets);
    // ▲▲▲ 添加调试日志 ▲▲▲

    // ▼▼▼ 【新增】为日志记录累加RTCP统计信息 ▼▼▼
    session.logIntervalSumThroughput += throughput;
    session.logIntervalSumDelay += avgDelay;
    session.logIntervalSumLossRate += lossRate;
    session.logIntervalRtcpCount++;
    // ▲▲▲ 【新增】为日志记录累加RTCP统计信息 ▲▲▲
    
    // 【至关重要】重置周期统计变量，并更新状态
    session.intervalReceivedPackets = 0;
    session.intervalReceivedBytes = 0;
    session.intervalTotalDelay = Seconds(0);
    session.lastReportedSentPackets = session.maxSeenSentPackets;
    session.lastReportTime = now;

    ScheduleReport(clientAddress);
}


// --- 新的播放和日志记录函数 ---

void YtyServer::SchedulePlayback(const Address& clientAddress)
{
    if (m_sessions.count(clientAddress)) {
        // 使用会话中存储的、协商好的帧率
        ClientSession& session = m_sessions[clientAddress];
        if (session.frameRate == 0) return; // 防止除以0

        Time playbackInterval = Seconds(1.0 / session.frameRate);
        m_sessions[clientAddress].playbackEvent = Simulator::Schedule(playbackInterval, &YtyServer::TryPlayback, this, clientAddress);
    }
}

void YtyServer::TryPlayback(const Address& clientAddress)
{
    if (!m_sessions.count(clientAddress)) return;

    ClientSession& session = m_sessions[clientAddress];
    uint32_t frameToPlay = session.nextFrameToPlay;
    
    // 检查帧是否存在于缓冲区中
    auto it = session.buffer.find(frameToPlay);
    if (it == session.buffer.end())
    {
        // 帧完全不存在。安排一个卡顿超时。
        if (!session.stutterTimeoutEvent.IsPending()) {
             session.stutterTimeoutEvent = Simulator::Schedule(MilliSeconds(50), &YtyServer::HandleStutter, this, clientAddress);
        }
        return; // 等待数据包或卡顿超时
    }

    // 帧存在，检查它是否完整。
    // 为此，我们需要知道这一帧总共有多少包。
    // 我们可以查看我们收到的该帧第一个包的头部信息。
    auto& packetsInFrameMap = it->second;
    Ptr<Packet> firstPacket = packetsInFrameMap.begin()->second.packet->Copy();
    RtpHeader header;
    firstPacket->RemoveHeader(header);
    uint32_t requiredPackets = header.GetPacketsInFrame();

    if (packetsInFrameMap.size() >= requiredPackets)
    {
        // 帧是完整的！
        NS_LOG_INFO("At time " << Simulator::Now().GetSeconds() << "s, Server PLAYED frame " << frameToPlay);
        
        // 取消为该帧设置的任何卡顿超时，因为它现在已经到达。
        if (session.stutterTimeoutEvent.IsPending()) {
            Simulator::Cancel(session.stutterTimeoutEvent);
        }

        session.playedFrames++;
        session.nextFrameToPlay++; // 移动到下一帧
        session.buffer.erase(frameToPlay); // 清理缓冲区

        // 安排下一次播放尝试。
        SchedulePlayback(clientAddress);
    }
    else
    {
        // 帧已开始到达但尚不完整。如果卡顿计时器还未运行，则启动它。
        if (!session.stutterTimeoutEvent.IsPending()) {
             session.stutterTimeoutEvent = Simulator::Schedule(MilliSeconds(50), &YtyServer::HandleStutter, this, clientAddress);
        }
    }
}

void YtyServer::HandleStutter(const Address& clientAddress)
{
    if (!m_sessions.count(clientAddress)) return;

    ClientSession& session = m_sessions[clientAddress];
    NS_LOG_INFO("At time " << Simulator::Now().GetSeconds() << "s, STUTTER detected for frame " << session.nextFrameToPlay << ". Skipping.");

    session.stutterEvents++;
    session.nextFrameToPlay++; // 跳过迟到的帧

    // 跳过之后，立即尝试播放下一帧。
    SchedulePlayback(clientAddress);
}


void YtyServer::ScheduleLog(const Address& clientAddress)
{
    if (m_sessions.count(clientAddress)) {
        m_sessions[clientAddress].logStatsEvent = Simulator::Schedule(m_logInterval, &YtyServer::LogPlaybackStats, this, clientAddress);
    }
}


void YtyServer::LogPlaybackStats(const Address& clientAddress)
{
    if (!m_sessions.count(clientAddress)) return;

    ClientSession& session = m_sessions[clientAddress];
    
    // --- 计算播放统计 ---
    double stutterRate = 0;
    // 分母是总的尝试播放帧数（已播放的 + 卡顿跳过的）
    if ((session.playedFrames + session.stutterEvents) > 0)
    {
        stutterRate = static_cast<double>(session.stutterEvents) / (session.playedFrames + session.stutterEvents);
    }

    // ▼▼▼ 【新增】计算包括抖动在内的各项指标平均值 ▼▼▼
    double avgThroughput = 0.0;
    double avgDelayMs = 0.0;
    double avgLossRate = 0.0;
    double avgJitterMs = 0.0; // 抖动平均值，单位毫秒

    if (session.logIntervalRtcpCount > 0)
    {
        avgThroughput = session.logIntervalSumThroughput / session.logIntervalRtcpCount;
        avgDelayMs = (session.logIntervalSumDelay.GetMilliSeconds()) / session.logIntervalRtcpCount;
        avgLossRate = session.logIntervalSumLossRate / session.logIntervalRtcpCount;
        avgJitterMs = (session.logIntervalSumJitter / session.logIntervalRtcpCount) * 1000.0; // 转换为毫秒
    }
    // ▲▲▲ 【新增】计算包括抖动在内的各项指标平均值 ▲▲▲

    // ▼▼▼ 【修改】将抖动值写入日志文件 ▼▼▼
    if (m_logFile.is_open())
    {
        m_logFile << Simulator::Now().GetSeconds() << "\t"
                  << InetSocketAddress::ConvertFrom(clientAddress).GetIpv4() << "\t"
                  << avgThroughput << "\t"
                  << avgDelayMs << "\t"
                  << avgLossRate << "\t"
                  << avgJitterMs << "\t" // 在丢包率后插入抖动值
                  << session.playedFrames << "\t"
                  << session.stutterEvents << "\t"
                  << stutterRate << "\t"
                  << session.clientInfo.cameraId << "\t"
                  << session.clientInfo.accessType << "\t"
                  << session.clientInfo.region << std::endl;
    }
    // ▲▲▲ 【修改】将抖动值写入日志文件 ▲▲▲
    
    // --- 为下一个统计周期重置所有日志相关的统计量 ---
    session.playedFrames = 0;
    session.stutterEvents = 0;

    // ▼▼▼ 【新增】重置所有日志相关的统计量，包括抖动 ▼▼▼
    session.logIntervalSumThroughput = 0.0;
    session.logIntervalSumDelay = Seconds(0);
    session.logIntervalSumLossRate = 0.0;
    session.logIntervalSumJitter = 0.0; // 重置抖动累加器
    session.logIntervalRtcpCount = 0;
    // ▲▲▲ 【新增】重置所有日志相关的统计量，包括抖动 ▲▲▲

    // 安排下一次日志事件
    ScheduleLog(clientAddress);
}

} // namespace ns3