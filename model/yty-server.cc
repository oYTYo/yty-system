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

#include "yty-camera.h" // 包含摄像头头文件以使用 RtpHeader

#include <algorithm>
#include <iomanip>


namespace ns3 {

NS_LOG_COMPONENT_DEFINE("YtyServerApplication");
NS_OBJECT_ENSURE_REGISTERED(YtyServer);


// 码率的绝对上限和下限，防止码率无限增长或低到无意义
const double MAX_BITRATE_MBPS = 10.0; // 码率最高不超过 10 Mbps
const double MIN_BITRATE_KBPS = 100.0; // 码率最低不低于 100 Kbps

// 单位换算常量
const double BPS_IN_KBPS = 1000.0; // 1 Kbps = 1000 bps

// 基于丢包控制的两个核心阈值
const double LOSS_RATE_THRESHOLD_LOW = 0.001;   // 丢包率低于 0.1% 时，网络被认为是“健康的”，允许码率增加
const double LOSS_RATE_THRESHOLD_HIGH = 0.01;   // 丢包率高于 1% 时，网络被认为是“严重拥塞的”，必须降低码率

// 基于延迟控制的核心初始阈值 (gamma)
const double OVERUSE_THRESHOLD_MS_INITIAL = 12.5; // 延迟变化趋势的初始判断阈值 (单位: 毫秒)

// 定义一个标准的数据包大小（字节），用于加性增的计算。
const double AVERAGE_PACKET_SIZE_BYTES = 1200.0;


// --- GCCController 构造函数实现 ---
// (这部分也要从 gcc_server.cpp 迁移过来)
GCCController::GCCController(double start_bitrate_kbps)
    : current_bitrate_bps_(start_bitrate_kbps * BPS_IN_KBPS),
      last_acked_bitrate_bps_(0.0),
      last_update_ms_(-1),
      last_group_arrival_time_ms_(-1),
      last_group_timestamp_ms_(-1),
      overuse_threshold_ms_(OVERUSE_THRESHOLD_MS_INITIAL),
      state_(NetworkState::Normal),
      time_of_last_bitrate_increase_ms_(-1) {}


// --- GCCController 方法实现 (精确复制自 gcc_server.cpp 并适配时间源) ---

std::string GCCController::loss_based_control(double loss_rate) {
    if (loss_rate > LOSS_RATE_THRESHOLD_HIGH) return "decrease";
    if (loss_rate < LOSS_RATE_THRESHOLD_LOW) return "increase";
    return "hold";
}

std::string GCCController::delay_based_control(double delay_ms, long long current_time_ms) {
    if (last_group_arrival_time_ms_ == -1) {
        last_group_arrival_time_ms_ = current_time_ms;
        last_group_timestamp_ms_ = current_time_ms - static_cast<long long>(delay_ms);
        return "hold";
    }

    long long arrival_delta_ms = current_time_ms - last_group_arrival_time_ms_;
    long long estimated_send_time_ms = current_time_ms - static_cast<long long>(delay_ms);
    long long timestamp_delta_ms = estimated_send_time_ms - last_group_timestamp_ms_;
    
    double delay_variation_ms = static_cast<double>(arrival_delta_ms - timestamp_delta_ms);
    
    last_group_arrival_time_ms_ = current_time_ms;
    last_group_timestamp_ms_ = estimated_send_time_ms;

    if (delay_variation_ms > overuse_threshold_ms_) {
        state_ = NetworkState::Overuse;
    } else if (std::abs(delay_variation_ms) < overuse_threshold_ms_) {
        state_ = NetworkState::Normal;
    } else { // delay_variation_ms < -overuse_threshold_ms_
        state_ = NetworkState::Underuse;
    }
    
    double K_u = (overuse_threshold_ms_ < 6) ? 0.01 : 0.00018;
    double K_d = 0.039;
    double update_rate = (std::abs(delay_variation_ms) > overuse_threshold_ms_) ? K_u : K_d;
    overuse_threshold_ms_ += (std::abs(delay_variation_ms) - overuse_threshold_ms_) * update_rate;
    overuse_threshold_ms_ = std::max(6.0, std::min(overuse_threshold_ms_, 600.0));

    if (state_ == NetworkState::Overuse) return "decrease";
    if (state_ == NetworkState::Normal) return "increase";
    if (state_ == NetworkState::Underuse) return "increase";
    
    return "hold";
}

// get_target_bitrate_kbps 接口适配 ns-3 参数
double GCCController::get_target_bitrate_kbps(double throughputKbps, double delayMs, double lossRate, double rttMs, long long currentTimeMs) {
    // 移除原始 gcc_server.cpp 中的 JSON 解析部分，直接使用传入的参数
    last_acked_bitrate_bps_ = throughputKbps * BPS_IN_KBPS;
    
    std::string loss_decision = loss_based_control(lossRate);
    std::string delay_decision = delay_based_control(delayMs, currentTimeMs);
    
    update_bitrate(loss_decision, delay_decision, rttMs, currentTimeMs);
    
    current_bitrate_bps_ = std::max(current_bitrate_bps_, MIN_BITRATE_KBPS * BPS_IN_KBPS);
    current_bitrate_bps_ = std::min(current_bitrate_bps_, MAX_BITRATE_MBPS * 1e6); // 1e6 是 MBPS 到 BPS

    return current_bitrate_bps_ / BPS_IN_KBPS;
}

std::string GCCController::get_state_string() const {
    switch (state_) {
        case NetworkState::Normal: return "Normal";
        case NetworkState::Overuse: return "Overuse";
        case NetworkState::Underuse: return "Underuse";
        default: return "Unknown";
    }
}

void GCCController::update_bitrate(const std::string& loss_decision, const std::string& delay_decision, double rtt_ms, long long current_time_ms) {
    if (loss_decision == "decrease" || delay_decision == "decrease") {
        current_bitrate_bps_ = std::min(
            current_bitrate_bps_,
            std::max(last_acked_bitrate_bps_ * 0.85, MIN_BITRATE_KBPS * BPS_IN_KBPS)
        );
        time_of_last_bitrate_increase_ms_ = -1;
        return;
    }

    if (loss_decision == "increase" && delay_decision == "increase") {
        if (state_ == NetworkState::Normal) {
            double time_delta_seconds = (last_update_ms_ > 0) ? 
                                        (current_time_ms - last_update_ms_) / 1000.0 : 
                                        0.02; 

            double response_time_ms = 100.0 + rtt_ms;
            // 还原为 gcc_server.cpp 中的值：0.5
            double alpha = 0.5 * time_delta_seconds; 
            // 还原为 gcc_server.cpp 中的值：50000.0
            double additive_increase_bps = std::max(50000.0, alpha * (AVERAGE_PACKET_SIZE_BYTES * 8000.0) / response_time_ms);
            
            current_bitrate_bps_ += additive_increase_bps;

        } else { // state_ == NetworkState::Underuse
            // 还原为 gcc_server.cpp 中的值：1.15
            current_bitrate_bps_ *= 1.15;
        }
    }
    last_update_ms_ = current_time_ms;
}


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
                      TimeValue(MilliSeconds(500)),
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
        m_logFile << "Time(s)\tClientAddr\tCameraId\tThroughput(kbps)\tAvgDelay(ms)\tAvgLossRate\tAvgJitter(ms)\tPlayedFrames\tStutterEvents\tStutterRate\tSkippedFrames\tDiscardedBytes(kb)\tAccessType\tRegion\tCodec\tAIBandwidth(kbps)\tResolution\tCRF\tActualBitrate(kbps)" << std::endl;
    }
}
    

void YtyServer::StopApplication(void)
{
    for (auto const& [addr, session] : m_sessions)
    {
        Simulator::Cancel(session.reportEvent);
        Simulator::Cancel(session.playbackEvent);
        Simulator::Cancel(session.stutterTimeoutEvent);
        Simulator::Cancel(session.logStatsEvent);
    }
    m_sessions.clear();

    if (m_logFile.is_open())
    {
        m_logFile.flush(); 
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

        // --- [最终的核心修正] ---
        // 我们需要一种方法来区分RTP包和文本控制包。
        // 之前的方法（在副本上调用RemoveHeader）仍然不安全，因为它在检查之前就尝试反序列化。
        // 正确且安全的方法是只读取第一个字节，检查它是否是我们的RTP魔数(0xAC)。

        // 检查包的大小是否至少为1字节，以安全地读取第一个字节
        if (packet->GetSize() >= 1)
        {
            uint8_t magicNumber;
            // CopyData是安全的，它只复制指定数量的字节到缓冲区，不会修改原始包
            packet->CopyData(&magicNumber, 1);

            // 检查这个字节是否是RTP包的魔数
            if (magicNumber == 0xAC)
            {
                // 魔数匹配，这几乎可以肯定是我们的RTP包。
                // 现在我们可以安全地把它交给RTP处理器，它会在内部调用RemoveHeader。
                ProcessRtp(packet, from);
            }
            else
            {
                // 魔数不匹配，这必定是一个文本控制包。
                // 按原样处理文本消息。
                uint8_t buffer[256];
                packet->CopyData(buffer, std::min((uint32_t)255, packet->GetSize()));
                buffer[std::min((uint32_t)255, packet->GetSize())] = '\0'; // 确保字符串正确终止
                std::string request(reinterpret_cast<char*>(buffer));

                // 现在，我们可以安全地根据字符串内容进行分发。
                if (request.rfind("PLAY", 0) == 0 || request.rfind("TEARDOWN", 0) == 0)
                {
                    ProcessRtsp(packet, from);
                }

                else if (request.rfind("SET_PARAMS", 0) == 0)
                {
                    // 处理来自摄像头的参数更新
                    if (m_sessions.count(from)) {
                        ClientSession& session = m_sessions[from];
                        std::istringstream requestStream(request);
                        std::string line;
                        while (std::getline(requestStream, line))
                        {
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
                else
                {
                     NS_LOG_WARN("收到一个未知类型的控制包，来自 " << InetSocketAddress::ConvertFrom(from).GetIpv4() << ", 内容: " << request);
                }
            }
        }
        else
        {
            // 包的大小甚至小于1字节，这不太可能发生，但作为健壮性检查，我们将其视为未知包。
            NS_LOG_WARN("收到一个大小小于1字节的包，已忽略。");
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
        session.discardedBytesDueToStutter += packet->GetSize(); // 累加过时丢弃的字节
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

  
    // 1. 计算在这个统计周期内，发送端总共发送了多少个包。
    //    maxSeenSentPackets 是这个周期内收到的RTP包头里最大的全局序号。
    //    lastReportedSentPackets 是上个周期记录的最大全局序号。
    //    它们的差值，就是这个周期内发送端发出的总包数。
    uint32_t intervalSent = session.maxSeenSentPackets - session.lastReportedSentPackets;

    double lossRate = 0.0;
    if (intervalSent > 0)
    {
        // intervalReceivedPackets 是这个周期内实际收到的总包数。
        // 这个值是通过在 ProcessRtp 中对每个到达的包计数得来的，是绝对准确的。
        lossRate = 1.0 - (double)session.intervalReceivedPackets / intervalSent;
    }
    // 安全检查，确保丢包率不会是负数（可能由于乱序导致 maxSeenSentPackets 更新延迟）
    if (lossRate < 0) lossRate = 0.0;
    
    // --- 后续的拥塞控制和发送反馈逻辑保持不变 ---
    Time avgDelay = (session.intervalReceivedPackets > 0) ? session.intervalTotalDelay / session.intervalReceivedPackets : Seconds(0);
    double bandwidthToReportKbps = session.lastThroughputKbpsForAI;
    if (lossRate < 0.001 && avgDelay < MilliSeconds(10))
    {
        double optimisticBw = std::max(session.actualBitrate / 1000.0, session.aiBandwidth / 1000.0) * 1.25;
        bandwidthToReportKbps = std::max(session.lastThroughputKbpsForAI, optimisticBw);
    }
    uint32_t aiBandwidth = GetBitrateFromGCC(session, bandwidthToReportKbps, avgDelay, lossRate);
    session.aiBandwidth = aiBandwidth;
    Ptr<Packet> rtcpPacket = Create<Packet>(reinterpret_cast<const uint8_t*>(&aiBandwidth), sizeof(uint32_t));
    m_socket->SendTo(rtcpPacket, 0, clientAddress);

    // --- 重置统计数据，为下一个周期做准备 ---
    session.logIntervalSumDelay += avgDelay;
    session.logIntervalSumLossRate += lossRate;
    session.logIntervalSumJitter += session.jitter;
    session.logIntervalRtcpCount++;

    session.intervalReceivedPackets = 0;
    session.intervalReceivedBytes = 0;
    session.intervalTotalDelay = Seconds(0);
    session.lastReportTime = now;
    
    // 将本周期看到的最大序列号，保存起来，作为下个周期的计算基准。
    session.lastReportedSentPackets = session.maxSeenSentPackets;

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
        // 累加被丢弃帧的字节数
        for (const auto& pair : session.buffer[frameToClean]) {
            session.discardedBytesDueToStutter += pair.second.packet->GetSize();
        }
        // 从 map 中删除这个永远不会被播放的帧的所有相关数据。
        session.buffer.erase(frameToClean);
        NS_LOG_INFO("从缓冲区中删掉帧 " << frameToClean << " 避免持续累积.");
    }

    session.stutterEvents++;
    session.skippedFramesDueToStutter++; // 统计跳过的帧数
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
                  << session.clientInfo.cameraId << "\t"  // CameraId
                  << throughputKbps << "\t"             // Throughput(kbps) - 直接使用已是Kbps单位的变量，无需再除1000
                  << avgDelayMs << "\t"                 // AvgDelay(ms)
                  << avgLossRate << "\t"                // AvgLossRate
                  << avgJitterMs << "\t"                // AvgJitter(ms)
                  << session.playedFrames << "\t"       // PlayedFrames
                  << session.stutterEvents << "\t"      // StutterEvents
                  << stutterRate << "\t"                 // StutterRate
                  << session.skippedFramesDueToStutter << "\t" // 跳过的帧数
                  << session.discardedBytesDueToStutter / 1000 << "\t" // 因卡顿丢弃的字节数 (kb)
                 
                  << session.clientInfo.accessType << "\t"// AccessType
                  << session.clientInfo.region << "\t"    // Region
                  << session.clientInfo.codec << "\t"     // Codec
                  << session.aiBandwidth / 1000 << "\t"   // AIBandwidth(kbps)
                  << session.resolution << "\t"           // Resolution
                  << session.crf << "\t"                  // CRF
                  << session.actualBitrate / 1000 << '\n'; // ActualBitrate(kbps)
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
    session.skippedFramesDueToStutter = 0;
    session.discardedBytesDueToStutter = 0;

    // 安排下一次日志事件
    ScheduleLog(clientAddress);
}


uint32_t YtyServer::GetBitrateFromGCC(ClientSession& session, double throughputKbps, Time delay, double lossRate)
{
    // 获取当前仿真时间（毫秒），作为 GCCController 的时间戳
    long long current_time_ms = Simulator::Now().GetMilliSeconds();

    // 直接调用 session 内部的 GCCController 实例来获取目标码率
    double target_kbps = session.gccController->get_target_bitrate_kbps(
                                throughputKbps,
                                delay.GetMilliSeconds(), // 将 ns3::Time 转换为毫秒
                                lossRate,
                                delay.GetMilliSeconds(), // 对于 GCC，RTT 也可以用当前延迟
                                current_time_ms
                            );
    
    // NS_LOG_INFO("回复摄像头 " << session.clientInfo.cameraId << ": 推荐码率 " << std::fixed << std::setprecision(2) << target_kbps << " Kbps, 网络状态: " << session.gccController->get_state_string());

    // 更新会话中存储的上次 AI 决策码率
    session.lastAiBitrateDecisionBps = static_cast<uint32_t>(target_kbps * 1000.0); // 将 Kbps 转换回 bps

    return session.lastAiBitrateDecisionBps;
}


}
// namespace ns3