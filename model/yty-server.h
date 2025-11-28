/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */

#ifndef YTY_SERVER_H
#define YTY_SERVER_H

#include "ns3/application.h"
#include "ns3/event-id.h"
#include "ns3/ptr.h"
#include "ns3/address.h"
#include "ns3/traced-callback.h"

#include "ns3/ipv4-address.h"

#include "zmq.hpp"

#include <map>
#include <vector>
#include <fstream>

#include <memory> 
#include <set>

#include "ns3/data-rate.h"

namespace ns3 {

class Socket;
class Packet;


// --- 定义 NetworkState ---
enum class NetworkState {
    Normal,   // 正常: 延迟稳定，可以缓慢增加码率
    Overuse,  // 过载: 延迟有增长趋势，必须降低码率
    Underuse  // 未充分利用: 延迟有降低趋势，可以更积极地增加码率
};

// 这个结构体不仅包含目标码率，还包含了我们需要的两个决策过程。
struct GCCResult {
    double target_bitrate_kbps; // 计算出的目标码率
    std::string loss_decision;  // 基于丢包的决策 ("increase", "decrease", "hold")
    std::string delay_decision; // 基于延迟的决策 ("increase", "decrease", "hold")
};


// 定义一个结构体来存储50ms的指标样本，这个结构体主要是收集传输层指标的序列给RL模型使用的
struct TransportMetricSample {
    double throughputKbps;
    double delayMs;
    double lossRate;
    double jitterMs;
};


// --- GCCController 类定义 ---
class GCCController {
public:
    // --- 构造函数 ---
    // start_bitrate_kbps 初始码率 (kbps)
    GCCController(double start_bitrate_kbps = 800.0); // 声明构造函数

    // --- 核心入口函数 ---
    // 适配 ns-3 的参数接口，不再接受 json
    GCCResult get_target_bitrate_kbps(double throughputKbps, double delayMs, double lossRate, double rttMs, long long currentTimeMs, double minervaWeight);
    
    // --- 辅助函数 ---
    void ResetState(double bitrate_kbps);
    std::string get_state_string() const;

private:
    // --- 内部成员变量 ---
    // (与 gcc_server.cpp 中的成员变量保持一致)
    double current_bitrate_bps_;
    double last_acked_bitrate_bps_;
    long long last_update_ms_;
    long long last_group_arrival_time_ms_;
    long long last_group_timestamp_ms_;
    double overuse_threshold_ms_;
    NetworkState state_;
    long long time_of_last_bitrate_increase_ms_;

    // --- GCC实现：内部辅助方法 ---
    std::string loss_based_control(double loss_rate);
    std::string delay_based_control(double delay_ms, long long current_time_ms);
    void update_bitrate(const std::string& loss_decision, const std::string& delay_decision, double rtt_ms, long long current_time_ms, double minervaWeight);
};



/**
 * @brief 一个接收视频流的服务器应用
 *
 * 这个类模拟一个接收服务器，可以接收来自多个摄像头的RTP流，
 * 统计网络指标，并通过RTCP反馈。
 */
class YtyServer : public Application
{
public:
    static TypeId GetTypeId(void);
    YtyServer();
    virtual ~YtyServer();

    //Minerva VMAF 查询函数,根据摄像头上报的分辨率和CRF，查询其对应的VMAF值
    double GetVmafForParams(const std::string& codec, int width, int height, int crf);

    /**
     * @brief 存储一个客户端网络接口的所有相关信息
     */
    struct ClientInfo {
        uint32_t    cameraId;
        std::string accessType;
        std::string region;
        std::string codec;

        ClientInfo() : cameraId(0), accessType("Unknown"), region("Unknown"), codec("Unknown") {}
        ClientInfo(uint32_t id, std::string type, std::string reg, std::string c) : cameraId(id), accessType(type), region(reg), codec(c) {}
    };

    /**
     * @brief 从仿真脚本中注册一个客户端IP地址及其关联信息
     * @param clientIp 客户端网络接口的IP地址
     * @param info 包含该接口所有元数据的结构体
     */
    void RegisterClientInfo(const Ipv4Address& clientIp, const ClientInfo& info);

    // 公共方法，用于从仿真脚本接收“神谕”（总带宽）
    void SetTotalBandwidth(DataRate totalBandwidth);


protected:
    virtual void DoDispose(void);

private:

    bool m_useOracle; // Oracle 模式的开关

    // Minerva 开关
    bool m_useMinerva; // Minerva 机制的开关

    // zmq通信
    bool m_useAI; // AI模式的开关
    std::unique_ptr<zmq::context_t> m_zmq_context; // ZMQ的全局上下文
    std::map<Address, std::unique_ptr<zmq::socket_t>> m_zmq_sockets; // 每个客户端一个独立的ZMQ socket

    virtual void StartApplication(void);
    virtual void StopApplication(void);

    // 用于保存接收到的数据包及其到达时间的结构体
    struct ReceivedPacket {
        Ptr<Packet> packet;
        Time receivedTime;
    };

    // 抖动缓冲：将帧序号映射到一个 "包序号 -> 收到的包" 的map
    using JitterBuffer = std::map<uint32_t, std::map<uint32_t, ReceivedPacket>>;


    // 客户端信息结构体
    struct ClientSession {
        uint64_t intervalReceivedPackets; // 本周期内收到的总包数
        uint64_t intervalReceivedBytes;   // 本周期内收到的总字节数
        Time     intervalTotalDelay;      // 本周期内累计的总时延
        uint32_t lastReportedSentPackets; // 上次报告时，摄像头已发送的总包数
        uint32_t maxSeenSentPackets;      // 本周期内，看到的最大已发送包序号
        Time     lastReportTime;          // 上次发送报告的时间
        EventId  reportEvent;             // 统计报告事件ID
        JitterBuffer buffer;
        uint32_t nextFrameToPlay;       // 我们期望播放的下一帧的序号
        EventId  playbackEvent;         // 触发下一次播放尝试的事件
        EventId  stutterTimeoutEvent;   // 处理帧未按时到达的事件
        Time     stutterTimeout;        // <<< 新增: 存储为该会话计算的卡顿超时时长
        uint32_t frameRate;             // 用于存储协商后的帧率
        uint32_t playedFrames;          // 当前1秒周期内播放的总帧数
        uint32_t stutterEvents;         // 当前1秒周期内的总卡顿次数
        EventId  logStatsEvent;         // 触发日志记录的事件
        bool     loggingStarted;
        bool     hasReceivedAPacket;
        Time     lastArrivalTime;       // 上一个RTP包的到达时间
        Time     lastSentTime;          // 上一个RTP包的发送时间
        double   jitter;                // 计算出的抖动值 (单位: 秒)
        Time     logIntervalSumDelay;      // 日志周期内，延迟的总和
        double   logIntervalSumLossRate;   // 日志周期内，丢包率的总和
        uint32_t logIntervalRtcpCount;     // 日志周期内，收到的RTCP包数量
        double   logIntervalSumJitter;      // 日志周期内，抖动的总和
        uint64_t logIntervalReceivedBytes;  // 日志周期内收到的总字节数
        Time     logIntervalStartTime;      // 日志周期的开始时间
        double   lastThroughputKbpsForAI;   // 上次计算出的吞-吐量(kbps)，供AI使用
        double   logIntervalSumAiBandwidthBps;     // 用于累加服务器建议的带宽 (bps)
        double   logIntervalSumActualBitrateBps;   // 用于累加摄像头实际上报的码率 (bps)
        double   logIntervalSumVmaf;               // 用于累加根据分辨率和CRF计算出的VMAF分数
        uint32_t logIntervalParamUpdateCount;      // 记录摄像头参数更新的次数，用于计算码率和VMAF的平均值
        double   logIntervalSumCrf;                // 用于累加CRF值
        double   logIntervalSumFrameRate;          // 用于累加编码帧率
        std::set<std::string> logIntervalResolutions; // 用于存储这1s内出现过的所有分辨率（自动去重）

        // 直接包含一个ClientInfo结构体 VVV
        ClientInfo clientInfo;

        std::string resolution;
        uint32_t    crf;
        uint32_t    actualBitrate; // 单位: bps
        uint32_t    aiBandwidth;   // 存储AI给出的建议带宽 (bps)
        uint32_t    lastAiBitrateDecisionBps;
        std::unique_ptr<GCCController> gccController;
        // Minerva 相关状态变量
        double   lastVMAF;              // 上一个周期的VMAF值，用于计算VMAF_Jitter
        double   qoeValue;              // 当前计算出的QoE值
        double   minervaWeight;         // 当前计算出的Minerva权重 w
        double   smoothedMinervaWeight;

        // AI强化学习所需的变量
        double   aiControlledWeight;    // 最终决策的权重 (AI或Minerva)，供50ms循环使用
        std::vector<TransportMetricSample> metricSamples; // 存储1秒内(20个)50ms的指标样本
        std::string lastAiStateJson;    // 存储上一个周期的聚合状态 (S_t)
        double   lastAiActionWeight;    // 存储上一个周期的AI动作 (A_t)

        // 构造函数
        ClientSession() :
            intervalReceivedPackets(0),
            intervalReceivedBytes(0),
            intervalTotalDelay(Seconds(0)),
            lastReportedSentPackets(0),
            maxSeenSentPackets(0),
            lastReportTime(Seconds(0)),
            nextFrameToPlay(0),
            stutterTimeout(MilliSeconds(40)),
            frameRate(30),
            playedFrames(0),
            stutterEvents(0),
            loggingStarted(false),
            hasReceivedAPacket(false),
            lastArrivalTime(Seconds(0)),
            lastSentTime(Seconds(0)),
            jitter(0.0),
            // logIntervalSumThroughput(0.0),
            logIntervalSumDelay(Seconds(0)),
            logIntervalSumLossRate(0.0),
            logIntervalRtcpCount(0),
            logIntervalSumJitter(0.0),
            logIntervalReceivedBytes(0),
            logIntervalStartTime(Seconds(0)),
            lastThroughputKbpsForAI(0.0),
            logIntervalSumAiBandwidthBps(0.0),
            logIntervalSumActualBitrateBps(0.0),
            logIntervalSumVmaf(0.0),
            logIntervalParamUpdateCount(0),
            logIntervalSumCrf(0.0),       // 初始化为0
            logIntervalSumFrameRate(0.0), // 初始化为0
            // logIntervalResolutions 不需要显式初始化，默认为空
            resolution("N/A"),
            crf(0),
            actualBitrate(0),
            aiBandwidth(0),
            lastAiBitrateDecisionBps(1000000),
            gccController(std::make_unique<GCCController>()),
            // 初始化 Minerva 相关变量
            lastVMAF(80.0), // 给予一个合理的初始值，避免第一次计算抖动过大
            qoeValue(0.0),
            minervaWeight(1.0), // 权重默认为1，即不产生影响
            smoothedMinervaWeight(1.0), // 平滑权重的初始值也设为1.0
            aiControlledWeight(1.0),         // 默认权重为1.0
            lastAiStateJson(""),             // 初始状态为空
            lastAiActionWeight(1.0)          // 初始动作权重为1.0
            
        {
        }
    };

    void HandleRead(Ptr<Socket> socket); // 处理接收到的数据包
    void ProcessRtp(Ptr<Packet> packet, const Address& from); // 处理RTP包
    void ProcessRtsp(Ptr<Packet> packet, const Address& from); // 处理RTSP包
    void ScheduleReport(const Address& clientAddress); // 调度统计报告
    void SendRtcpFeedback(const Address& clientAddress); // 发送RTCP反馈

    // 播放逻辑
    void SchedulePlayback(const Address& clientAddress);
    void TryPlayback(const Address& clientAddress);
    void HandleStutter(const Address& clientAddress);

    // 日志逻辑
    void ScheduleLog(const Address& clientAddress);
    void LogPlaybackStats(const Address& clientAddress);


    Ptr<Socket> m_socket;      // 服务器的Socket
    uint16_t m_port;           // 监听的端口
    Time m_reportInterval;     // 统计报告的间隔

    // 存储每个客户端会话的map，key是客户端地址
    std::map<Address, ClientSession> m_sessions;

    // 日志记录
    std::string m_logFileName;
    std::ofstream m_logFile;
    Time m_logInterval;

    // 追踪特定摄像头对拥塞控制算法的输入输出
    uint32_t m_traceCameraId;       // 要追踪的摄像头ID
    std::ofstream m_traceLogFile;   // 追踪日志的文件流

    // Oracle 模式所需的成员变量
    DataRate m_totalOracleBandwidth;            // 存储“神谕”告知的总带宽
    double m_totalCodecWeight;                  // 所有已连接客户端的总权重
    std::map<std::string, uint32_t> m_codecCounts;     // 每种编码器的客户端数量
    std::map<std::string, double> m_codecWeights;      // 每种编码器的权重 H.264: 2.0, H.265: 1.3 ...

    // 将每个客户端的IP地址映射到其完整的元数据
    std::map<Ipv4Address, ClientInfo> m_clientInfoRegistry;
  
    // 不再仅仅是从GCC获取，而是获取最终的目标码率
    uint32_t GetTargetBitrate(ClientSession& session, double throughputKbps, Time delay, double lossRate);
    
    // 一个专门用于和Python AI通信的函数
    uint32_t GetBitrateFromAI(ClientSession& session, double throughputKbps, Time delay, double lossRate, const Address& from);

    // VMAF查询表的私有成员, 使用嵌套 map 来存储 VMAF LUT
    std::map<std::string, std::map<std::pair<int, int>, std::map<int, double>>> m_vmafLut;
    void InitializeVmafLut(); // 用于初始化VMAF查询表的函数
    
};

} // namespace ns3

#endif /* YTY_SERVER_H */