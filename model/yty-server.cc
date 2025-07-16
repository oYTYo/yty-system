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

#include <nlohmann/json.hpp> // <<< 新增: 需要一个json库，推荐 nlohmann/json,// 您需要将其头文件放到ns-3可以找到的目录// 例如，下载 json.hpp 并放在 /usr/local/include/

#include "yty-camera.h" // 包含摄像头头文件以使用 RtpHeader

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
                      TimeValue(MilliSeconds(1000)),
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

YtyServer::YtyServer() : m_socket(0), m_port(9) {
    // <<< 新增: 初始化ZMQ上下文 >>>
    m_zmq_context = std::make_unique<zmq::context_t>(1);
}

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
                << ", Region=" << info.region
                
                << ", Codec=" << info.codec);
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

    m_logFile.open(m_logFileName, std::ios::out | std::ios::trunc);
    if (m_logFile.is_open())
    {
        m_logFile << "Time(s)\tClientAddr\tThroughput(kbps)\tAvgDelay(ms)\tAvgLossRate\tAvgJitter(ms)\tPlayedFrames\tStutterEvents\tStutterRate\tCameraId\tAccessType\tRegion\tCodec\tAIBandwidth(kbps)\tResolution\tCRF\tActualBitrate(kbps)" << std::endl;
    }
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
        // 如果是空包则跳过
        if (packet->GetSize() == 0) continue;

        // 1. 创建一个临时的包副本（Copy）用于检查
        Ptr<Packet> packetCopy = packet->Copy();
        
        // 2. 尝试从【副本】中解析出我们的自定义RTP头
        RtpHeader rtpHeader;
        uint32_t headerSize = packetCopy->RemoveHeader(rtpHeader);

        // 3. 检查头部是否成功解析，并且魔数是否匹配
        //    如果 headerSize > 0，说明成功解析出了一个头。
        if (headerSize > 0 && rtpHeader.GetMagic() == 0xAC)
        {
             // 确认是RTP包，将【原始包】交给RTP处理器
             ProcessRtp(packet, from);
        }
        else // 4. 如果不是我们定义的RTP包，那它一定是文本控制协议包
        {
            // 从原始包中读取文本内容
            uint8_t buffer[256]; // 缓冲区给大一点以防万一
            packet->CopyData(buffer, std::min((uint32_t)255, packet->GetSize()));
            buffer[std::min((uint32_t)255, packet->GetSize())] = '\0';
            std::string request(reinterpret_cast<char*>(buffer));
            
            // 根据请求的字符串内容进行分发
            if (request.rfind("PLAY", 0) == 0 || request.rfind("TEARDOWN", 0) == 0) {
                // 是标准RTSP请求，将【原始包】交给RTSP处理器
                ProcessRtsp(packet, from);
            }
            else if (request.rfind("SET_PARAMS", 0) == 0) {
                // 是我们自定义的 SET_PARAMS 请求，直接在此处理
                if (m_sessions.count(from)) {
                    ClientSession& session = m_sessions[from];
                    
                    // NS_LOG_INFO("At time " << Simulator::Now().GetSeconds() << "s, Server received SET_PARAMS from " << InetSocketAddress::ConvertFrom(from).GetIpv4());

                    // --- 【核心修正】使用更健壮的解析逻辑 ---
                    std::istringstream requestStream(request);
                    std::string line;
                    while (std::getline(requestStream, line))
                    {
                        // 去除行尾的 \r 
                        if (!line.empty() && line.back() == '\r') {
                            line.pop_back();
                        }

                        std::string header_res = "X-Resolution: ";
                        std::string header_crf = "X-CRF: ";
                        std::string header_br = "X-Actual-Bitrate: ";

                        if (line.rfind(header_res, 0) == 0) {
                            session.resolution = line.substr(header_res.length());
                        }
                        else if (line.rfind(header_crf, 0) == 0) {
                            session.crf = std::stoul(line.substr(header_crf.length()));
                        }
                        else if (line.rfind(header_br, 0) == 0) {
                            session.actualBitrate = std::stoul(line.substr(header_br.length()));
                        }
                    }
                   
                }
            }
            else {
                 NS_LOG_WARN("Received an unknown control packet from " << InetSocketAddress::ConvertFrom(from).GetIpv4() << ", content: " << request);
            }
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

    // 创建一个副本用于读取头，因为 RemoveHeader 会修改原始包
    Ptr<Packet> packetCopy = packet->Copy();
    RtpHeader rtpHeader;
    packetCopy->RemoveHeader(rtpHeader);

    // 在缓冲数据包之前，检查它是否已经过时。
    uint32_t frameSeq = rtpHeader.GetFrameSeq();
    if (frameSeq < session.nextFrameToPlay)
    {
        // NS_LOG_INFO("At time " << Simulator::Now().GetSeconds() << "s, Server discarded an old packet for frame " << frameSeq << " (expecting frame " << session.nextFrameToPlay << ").");
        return; // 丢弃过时的包，函数直接返回
    }


    session.hasReceivedAPacket = true;

    // 这确保了日志只在数据真实流动后才开始，消除了初始的零值垃圾数据。
    if (!session.loggingStarted)
    {
        // NS_LOG_INFO("First RTP packet received from " << InetSocketAddress::ConvertFrom(from).GetIpv4() << ". Starting periodic logging for this session.");

        // +++ 当日志首次启动时，记录当前时间作为日志周期的起点 +++
        session.logIntervalStartTime = Simulator::Now();

        ScheduleLog(from);          // 启动日志记录循环
        session.loggingStarted = true; // 设置标志，防止重复启动
    }

    uint32_t packetSize = packet->GetSize();
    Time now = Simulator::Now();

    Time sentTime = NanoSeconds(rtpHeader.GetTimestamp());
    Time delay = now - sentTime;
    
    // 更新网络统计
    session.intervalReceivedPackets++;
    session.intervalReceivedBytes += packetSize;
    session.intervalTotalDelay += delay;

    session.logIntervalReceivedBytes += packetSize;

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

            // 从注册表查找信息
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

            // 给一个还不错的初始吞吐量
            session.lastThroughputKbpsForAI = 2000.0;


            // +++ VVV 新增: 为新会话创建并连接ZMQ socket +++
            NS_LOG_INFO("为新客户端 " << clientIp << " 创建ZMQ连接...");
            session.zmq_socket = std::make_unique<zmq::socket_t>(*m_zmq_context, zmq::socket_type::req);
            try {
                session.zmq_socket->connect("tcp://localhost:5557");
            } catch(const zmq::error_t& e) {
                NS_LOG_ERROR("ZMQ连接失败: " << e.what());
            }
            // +++ ^^^ 新增 ^^^ +++


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

                    // 在这里计算并存储超时时长 ▼▼▼
                    if (negotiatedRate > 0) {
                        // 使用1.5倍帧间隔作为超时，增加网络抖动容忍度
                        m_sessions[from].stutterTimeout = MilliSeconds(2000 / negotiatedRate);
                    }

                    // 如果帧率发生变化，则立即重置播放调度
                    if (session.playbackEvent.IsPending() && negotiatedRate > 0)
                    {
                        // 如果播放事件正在运行（意味着这不是第一次PLAY），并且我们收到了一个有效的新帧率
                        NS_LOG_INFO("Frame rate changed for " << clientIp << ". Rescheduling playback event.");
                        Simulator::Cancel(session.playbackEvent); // 取消基于旧帧率的播放计划
                        session.playbackEvent = Simulator::Schedule(Seconds(1.0 / negotiatedRate), &YtyServer::TryPlayback, this, from); // 立即用新帧率安排下一次播放
                    }
                    
                } catch (const std::exception& e) {
                    NS_LOG_WARN("没有成功解析出帧率，使用默认值: " << m_sessions[from].frameRate);
                }
            }
            else
            {
                NS_LOG_INFO("报头没有帧率，使用默认值: " << m_sessions[from].frameRate);
            }
            // --- 解析结束 ---

            // --- 【新增】解析Codec ---
            std::string codec_header_key = "X-Codec: ";
            size_t codec_pos = request.find(codec_header_key);
            if (codec_pos != std::string::npos)
            {
                size_t end_pos = request.find("\r\n", codec_pos);
                m_sessions[from].clientInfo.codec = request.substr(codec_pos + codec_header_key.length(), end_pos - (codec_pos + codec_header_key.length()));
                NS_LOG_INFO("Negotiated codec with " << InetSocketAddress::ConvertFrom(from).GetIpv4() << ": " << m_sessions[from].clientInfo.codec);
            }
            else
            {
                // 如果请求中没有codec信息，则使用注册时提供的信息
                NS_LOG_INFO("No codec header found. Using registered codec: " << m_sessions[from].clientInfo.codec);
            }
            // --- 解析结束 ---

            // --- 启动播放和日志记录 ---
            SchedulePlayback(from);
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

    // --- 1. 计算网络状态 (与之前相同) ---
    Time avgDelay = (session.intervalReceivedPackets > 0) ? session.intervalTotalDelay / session.intervalReceivedPackets : Seconds(0);
    uint32_t intervalSent = session.maxSeenSentPackets - session.lastReportedSentPackets;
    double lossRate = 0.0;
    if (intervalSent > 0)
    {
        uint64_t receivedInInterval = std::min((uint64_t)intervalSent, session.intervalReceivedPackets);
        lossRate = 1.0 - (double)receivedInInterval / intervalSent;
    }
    if (lossRate < 0) lossRate = 0.0;

    // --- 2. 【新增】乐观带宽探测逻辑 ---
    // 定义触发探测的阈值
    const double PROBE_LOSS_RATE_THRESHOLD = 0.001; // 丢包率低于 0.1%
    const Time   PROBE_DELAY_THRESHOLD     = MilliSeconds(10); // 延迟低于 10ms
    
    // 从摄像头最新的参数报告中获取其当前的发送码率
    double currentActualBitrateKbps = session.actualBitrate / 1000.0;

    // 获取AI上一次给出的建议带宽
    double lastAiBandwidthKbps = session.aiBandwidth / 1000.0;

    // 决定本次要报告给AI的带宽值
    double bandwidthToReportKbps = session.lastThroughputKbpsForAI;
    
    // 检查是否满足“信息饥饿”条件
    if (lossRate < PROBE_LOSS_RATE_THRESHOLD && avgDelay < PROBE_DELAY_THRESHOLD)
    {
        // 网络状况极好，但吞吐量可能很低，需要主动探测
        // NS_LOG_INFO("Camera " << session.clientInfo.cameraId << ": Network is perfect (loss=" << lossRate << ", delay=" << avgDelay.GetMilliSeconds() << "ms). Activating optimistic probing.");
        
        // 我们选择一个更激进的值来报告给AI，鼓励它提速
        // 这个值可以是当前摄像头实际发送码率的1.25倍，或者是AI上次建议带宽的1.25倍，取较大者
        double optimisticBw = std::max(currentActualBitrateKbps, lastAiBandwidthKbps) * 1.25;

        // 确保探测值至少比当前测得的吞吐量大
        bandwidthToReportKbps = std::max(session.lastThroughputKbpsForAI, optimisticBw);
    }

    // --- 3. 调用AI模块获取可用带宽，注意：传入的是我们处理过的带宽值 ---
    uint32_t aiBandwidth = GetBitrateFromAI(session, bandwidthToReportKbps, avgDelay, lossRate);

    // +++ 将AI给出的带宽建议存入会话，以便日志记录 +++
    session.aiBandwidth = aiBandwidth;

    // --- 4. 将服务器计算出的【可用带宽】发送回摄像头 ---
    Ptr<Packet> rtcpPacket = Create<Packet>(reinterpret_cast<const uint8_t*>(&aiBandwidth), sizeof(uint32_t));

    m_socket->SendTo(rtcpPacket, 0, clientAddress);


    // 将当前计算出的指标累加到日志统计变量中
    session.logIntervalSumDelay += avgDelay;
    session.logIntervalSumLossRate += lossRate;
    session.logIntervalSumJitter += session.jitter; // 累加当前计算的抖动值
    session.logIntervalRtcpCount++;


    // --- 4. 重置周期统计数据 (与之前相同) ---
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
             session.stutterTimeoutEvent = Simulator::Schedule(session.stutterTimeout, &YtyServer::HandleStutter, this, clientAddress);
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
             session.stutterTimeoutEvent = Simulator::Schedule(session.stutterTimeout, &YtyServer::HandleStutter, this, clientAddress);
        }
    }
}

void YtyServer::HandleStutter(const Address& clientAddress)
{
    if (!m_sessions.count(clientAddress)) return;

    ClientSession& session = m_sessions[clientAddress];
    NS_LOG_INFO("At time " << Simulator::Now().GetSeconds() << "s, STUTTER detected for frame " << session.nextFrameToPlay << ". Skipping.");

    
    // 在跳过这一帧之前，必须将其已缓存的数据从抖动缓冲区中清除。这是防止数据结构无限增长、导致仿真速度变慢的关键。
    uint32_t frameToClean = session.nextFrameToPlay;
    if (session.buffer.count(frameToClean))
    {
        // 从 map 中删除这个永远不会被播放的帧的所有相关数据。
        session.buffer.erase(frameToClean);
        NS_LOG_INFO("从缓冲区中删掉帧 " << frameToClean << " 避免持续累积.");
    }
    // ====================== 【核心修正】 结束 ======================

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
    if ((session.playedFrames + session.stutterEvents) > 0)
    {
        stutterRate = static_cast<double>(session.stutterEvents) / (session.playedFrames + session.stutterEvents);
    }

    // --- 在此统一计算1秒日志周期的各项指标 ---
    Time logIntervalDuration = Simulator::Now() - session.logIntervalStartTime;
    double throughputKbps = 0.0;
    // 确保时长大于0，避免除零错误
    if (logIntervalDuration.GetSeconds() > 0)
    {
        // 计算吞吐量，单位是 Kbps
        // (字节 * 8.0) -> 比特; (/ 时长) -> bps; (/ 1000.0) -> Kbps
        throughputKbps = (session.logIntervalReceivedBytes * 8.0) / logIntervalDuration.GetSeconds() / 1000.0;
    }
    // 更新供AI模块使用的缓存值
    session.lastThroughputKbpsForAI = throughputKbps;

    // 计算其他指标的平均值
    double avgDelayMs = 0.0;
    double avgLossRate = 0.0;
    double avgJitterMs = 0.0;
    if (session.logIntervalRtcpCount > 0)
    {
        avgDelayMs = (session.logIntervalSumDelay.GetMilliSeconds()) / session.logIntervalRtcpCount;
        avgLossRate = session.logIntervalSumLossRate / session.logIntervalRtcpCount;
        avgJitterMs = (session.logIntervalSumJitter / session.logIntervalRtcpCount) * 1000.0; // 转换为毫秒
    }

    // --- 【核心修正】将计算好的各项指标写入日志文件 ---
    if (m_logFile.is_open())
    {
        m_logFile << Simulator::Now().GetSeconds() << "\t"
                  << InetSocketAddress::ConvertFrom(clientAddress).GetIpv4() << "\t" // ClientAddr
                  // ▼▼▼ 请确认此行代码 ▼▼▼
                  << throughputKbps << "\t"             // Throughput(kbps) - 直接使用已是Kbps单位的变量，无需再除1000
                  << avgDelayMs << "\t"                 // AvgDelay(ms)
                  << avgLossRate << "\t"                // AvgLossRate
                  << avgJitterMs << "\t"                // AvgJitter(ms)
                  << session.playedFrames << "\t"       // PlayedFrames
                  << session.stutterEvents << "\t"      // StutterEvents
                  << stutterRate << "\t"                 // StutterRate
                  << session.clientInfo.cameraId << "\t"  // CameraId
                  << session.clientInfo.accessType << "\t"// AccessType
                  << session.clientInfo.region << "\t"    // Region
                  << session.clientInfo.codec << "\t"     // Codec
                  << session.aiBandwidth / 1000 << "\t"   // AIBandwidth(kbps)
                  << session.resolution << "\t"           // Resolution
                  << session.crf << "\t"                  // CRF
                  << session.actualBitrate / 1000 << std::endl; // ActualBitrate(kbps)
    }
    
    // --- 为下一个日志周期重置所有相关的统计量 ---
    session.playedFrames = 0;
    session.stutterEvents = 0;
    session.logIntervalSumDelay = Seconds(0);
    session.logIntervalSumLossRate = 0.0;
    session.logIntervalSumJitter = 0.0;
    session.logIntervalRtcpCount = 0;
    session.logIntervalReceivedBytes = 0;
    session.logIntervalStartTime = Simulator::Now();

    // 安排下一次日志事件
    ScheduleLog(clientAddress);
}




// 在 yty-server.cc 文件中
uint32_t YtyServer::GetBitrateFromAI(ClientSession& session, double bandwidthKbps, Time delay, double lossRate)
{
    // 定义一个超时时间，如果一个请求超过这个时间没有回复，我们就认为它丢失了
    const Time ZMQ_REQUEST_TIMEOUT = MilliSeconds(500);
    Time now = Simulator::Now();

    // 1. 无论如何，都先尝试接收一次，看之前是否有未收到的回复
    // 这个操作只在 isWaitingForZmqReply 为 true 时有意义
    if (session.isWaitingForZmqReply)
    {
        try {
            zmq::message_t reply_msg;
            if (session.zmq_socket->recv(reply_msg, zmq::recv_flags::dontwait).has_value()) {
                // 如果成功收到回复，解析它并更新我们的状态
                std::string reply_str(static_cast<char*>(reply_msg.data()), reply_msg.size());
                auto reply_json = nlohmann::json::parse(reply_str);
                uint32_t newBitrate = reply_json.at("targetBitrate").get<uint32_t>();
                session.lastAiBitrateDecisionBps = newBitrate;
                
                // 关键：将等待状态置为 false，因为我们已经收到了回复
                session.isWaitingForZmqReply = false;
            }
        } catch (const zmq::error_t& e) {
            if (e.num() != EAGAIN) { // EAGAIN 是非阻塞模式下的正常“无消息”错误，不用打印
                 NS_LOG_ERROR("ZMQ recv error for camera " << session.clientInfo.cameraId << ": " << e.what());
            }
        } catch (const nlohmann::json::exception& e) {
            NS_LOG_ERROR("JSON解析错误: " << e.what());
        }
    }
    
    // 2. 检查是否应该发送一个新的请求
    bool shouldSend = false;
    if (session.isWaitingForZmqReply) {
        // 如果我们仍在等待一个回复，检查它是否超时
        if (now > session.lastZmqRequestTime + ZMQ_REQUEST_TIMEOUT) {
          
            // 请求超时了！我们必须销毁并重建套接字来重置ZMQ的状态机。
            // NS_LOG_INFO("ZMQ request for cam " << session.clientInfo.cameraId << " timed out. Resetting ZMQ socket.");

            // 销毁旧的套接字
            session.zmq_socket->close();
            // 创建一个全新的套接字
            session.zmq_socket = std::make_unique<zmq::socket_t>(*m_zmq_context, zmq::socket_type::req);
            
            // 【重要】为新套接字设置一个合理的超时，防止send/recv无限期阻塞 (虽然我们用的是非阻塞)
            int timeout_ms = 200; // 200ms
            session.zmq_socket->set(zmq::sockopt::rcvtimeo, timeout_ms);
            session.zmq_socket->set(zmq::sockopt::sndtimeo, timeout_ms);

            // 重新连接
            try {
                session.zmq_socket->connect("tcp://localhost:5557");
            } catch(const zmq::error_t& e) {
                NS_LOG_ERROR("ZMQ (re)connection failed: " << e.what());
            }

            // 重置状态，允许在新的套接字上发送请求
            session.isWaitingForZmqReply = false;
            shouldSend = true;
          
        }
    } else {
        // 如果我们没有在等待回复，那就可以自由发送
        shouldSend = true;
    }

    // 3. 如果决定要发送，就执行发送操作
    if (shouldSend) {
        nlohmann::json request_json;
        request_json["cameraId"] = session.clientInfo.cameraId;
        request_json["throughputKbps"] = bandwidthKbps;
        request_json["delayMs"] = delay.GetMilliSeconds();
        request_json["lossRate"] = lossRate;
        std::string request_str = request_json.dump();

        try {
            zmq::message_t request_msg(request_str.begin(), request_str.end());
            // 发送后，立刻进入等待状态，并记录发送时间
            if(session.zmq_socket->send(request_msg, zmq::send_flags::dontwait)) {
                session.isWaitingForZmqReply = true;
                session.lastZmqRequestTime = now;
            }
        } catch (const zmq::error_t& e) {
            NS_LOG_WARN("ZMQ send failed for camera " << session.clientInfo.cameraId << ": " << e.what());
        }
    }
    
    // 4. 无论本次操作如何，都返回AI给出的上一个有效决策
    return session.lastAiBitrateDecisionBps;
}



}
// namespace ns3