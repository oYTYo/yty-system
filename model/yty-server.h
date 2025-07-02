/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */

#ifndef YTY_SERVER_H
#define YTY_SERVER_H

#include "ns3/application.h"
#include "ns3/event-id.h"
#include "ns3/ptr.h"
#include "ns3/address.h"
#include "ns3/traced-callback.h"
#include "yty-camera.h" // 需要包含摄像头头文件以使用RtpHeader
#include "ns3/ipv4-address.h" // <<< 新增: 包含IPv4地址头文件

#include <map>
#include <vector>
#include <fstream> // 为 std::ofstream 添加此头文件

namespace ns3 {

class Socket;
class Packet;

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

    // VVV 新增: 客户端元数据结构体和注册方法 VVV
    /**
     * @brief 存储一个客户端网络接口的所有相关信息
     */
    struct ClientInfo {
        uint32_t    cameraId;
        std::string accessType;
        std::string region;
        std::string codec; // <<< 【新增】存储编码格式

        ClientInfo() : cameraId(0), accessType("Unknown"), region("Unknown"), codec("Unknown") {} // <<< 【修改】构造函数初始化
        ClientInfo(uint32_t id, std::string type, std::string reg, std::string c) // <<< 【修改】构造函数
        : cameraId(id), accessType(type), region(reg), codec(c) {}
    };

    /**
     * @brief 从仿真脚本中注册一个客户端IP地址及其关联信息
     * @param clientIp 客户端网络接口的IP地址
     * @param info 包含该接口所有元数据的结构体
     */
    void RegisterClientInfo(const Ipv4Address& clientIp, const ClientInfo& info);
    // ^^^ 新增 ^^^

protected:
    virtual void DoDispose(void);

private:
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
        // --- 用于当前报告周期的统计变量 ---
        uint64_t intervalReceivedPackets; // 本周期内收到的总包数
        uint64_t intervalReceivedBytes;   // 本周期内收到的总字节数
        Time     intervalTotalDelay;      // 本周期内累计的总时延

        // --- 用于丢包率计算的状态变量 ---
        uint32_t lastReportedSentPackets; // 上次报告时，摄像头已发送的总包数
        uint32_t maxSeenSentPackets;      // 本周期内，看到的最大已发送包序号

        // --- 计时和事件调度 ---
        Time     lastReportTime;          // 上次发送报告的时间
        EventId  reportEvent;             // 统计报告事件ID

        // --- 播放和抖动缓冲 ---
        JitterBuffer buffer;
        uint32_t nextFrameToPlay;       // 我们期望播放的下一帧的序号
        EventId  playbackEvent;         // 触发下一次播放尝试的事件
        EventId  stutterTimeoutEvent;   // 处理帧未按时到达的事件
        Time     stutterTimeout;        // <<< 新增: 存储为该会话计算的卡顿超时时长
        uint32_t frameRate;             // 用于存储协商后的帧率

        // --- 播放日志统计 ---
        uint32_t playedFrames;          // 当前1秒周期内播放的总帧数
        uint32_t stutterEvents;         // 当前1秒周期内的总卡顿次数
        EventId  logStatsEvent;         // 触发日志记录的事件

        // --- VVV 新增：用于抖动计算的状态变量 VVV ---
        Time     lastArrivalTime;       // 上一个RTP包的到达时间
        Time     lastSentTime;          // 上一个RTP包的发送时间
        double   jitter;                // 计算出的抖动值 (单位: 秒)
        // --- ^^^ 新增 ^^^ ---

        // ▼▼▼ 【新增】用于日志记录的RTCP指标累加器 ▼▼▼
        double   logIntervalSumThroughput; // 日志周期内，吞吐率的总和
        Time     logIntervalSumDelay;      // 日志周期内，延迟的总和
        double   logIntervalSumLossRate;   // 日志周期内，丢包率的总和
        uint32_t logIntervalRtcpCount;     // 日志周期内，收到的RTCP包数量
        // ▲▲▲ 【新增】用于日志记录的RTCP指标累加器 ▲▲▲

        // --- VVV 新增：用于抖动日志的累加器 VVV ---
        double   logIntervalSumJitter;      // 日志周期内，抖动的总和
        // --- ^^^ 新增 ^^^ ---

        // VVV 修改: 直接包含一个ClientInfo结构体 VVV
        ClientInfo clientInfo;
        // ^^^ 修改 ^^^

        // 构造函数
        ClientSession() :
            intervalReceivedPackets(0),
            intervalReceivedBytes(0),
            intervalTotalDelay(Seconds(0)),
            lastReportedSentPackets(0),
            maxSeenSentPackets(0),
            lastReportTime(Seconds(0)),
            nextFrameToPlay(0),
            stutterTimeout(MilliSeconds(40)), // <<< 新增: 给予一个默认值
            frameRate(30), // 给一个默认值, 以防协商失败
            playedFrames(0),
            stutterEvents(0),
            // --- VVV 新增：初始化新增的抖动相关成员变量 VVV ---
            lastArrivalTime(Seconds(0)),
            lastSentTime(Seconds(0)),
            jitter(0.0),
            // --- ^^^ 新增 ^^^ ---
            

            // ▼▼▼ 【新增】初始化新增的成员变量 ▼▼▼
            logIntervalSumThroughput(0.0),
            logIntervalSumDelay(Seconds(0)),
            logIntervalSumLossRate(0.0),
            logIntervalRtcpCount(0),
            // ▲▲▲ 【新增】初始化新增的成员变量 ▲▲▲

            // --- VVV 新增：初始化抖动累加器 VVV ---
            logIntervalSumJitter(0.0)
            // --- ^^^ 新增 ^^^ ---
            
        {
        }
    };

    void HandleRead(Ptr<Socket> socket); // 处理接收到的数据包
    void ProcessRtp(Ptr<Packet> packet, const Address& from); // 处理RTP包
    void ProcessRtsp(Ptr<Packet> packet, const Address& from); // 处理RTSP包
    void ScheduleReport(const Address& clientAddress); // 调度统计报告
    void SendRtcpFeedback(const Address& clientAddress); // 发送RTCP反馈
    void Playback(const Address& clientAddress);      // 模拟播放

    // 播放逻辑
    void SchedulePlayback(const Address& clientAddress);
    void TryPlayback(const Address& clientAddress);
    void HandleStutter(const Address& clientAddress);

    // 日志逻辑
    void ScheduleLog(const Address& clientAddress);
    void LogPlaybackStats(const Address& clientAddress);

    uint32_t m_cameraId;    // 摄像头的唯一ID

    Ptr<Socket> m_socket;      // 服务器的Socket
    uint16_t m_port;           // 监听的端口
    Time m_reportInterval;     // 统计报告的间隔

    // 存储每个客户端会话的map，key是客户端地址
    std::map<Address, ClientSession> m_sessions;

    // 日志记录
    std::string m_logFileName;
    std::ofstream m_logFile;
    Time m_logInterval;

    // VVV 新增: 客户端信息注册表 VVV
    // 将每个客户端的IP地址映射到其完整的元数据
    std::map<Ipv4Address, ClientInfo> m_clientInfoRegistry;
    // ^^^ 新增 ^^^
};

} // namespace ns3

#endif /* YTY_SERVER_H */