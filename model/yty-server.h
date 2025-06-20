/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */

#ifndef YTY_SERVER_H
#define YTY_SERVER_H

#include "ns3/application.h"
#include "ns3/event-id.h"
#include "ns3/ptr.h"
#include "ns3/address.h"
#include "ns3/traced-callback.h"
#include "yty-camera.h" // 需要包含摄像头头文件以使用RtpHeader

#include <map>
#include <vector>

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

protected:
    virtual void DoDispose(void);

private:
    virtual void StartApplication(void);
    virtual void StopApplication(void);

    // 客户端信息结构体
        struct ClientSession {
        // 接收缓冲区
        std::map<uint32_t, std::vector<Ptr<Packet>>> recvBuffer; 
        
        // --- 统计变量 ---
        // 当前周期的累计值
        uint64_t currentTotalReceivedPackets; // 当前周期累计收到的总包数
        uint64_t currentTotalReceivedBytes;   // 当前周期累计收到的总字节数
        Time     currentTotalDelay;           // 当前周期累计的总时延

        // 上一个周期的累计值 (用于计算差值)
        uint64_t lastTotalReceivedPackets;    // 上一周期记录的总包数
        uint64_t lastTotalReceivedBytes;      // 上一周期记录的总字节数
        Time     lastTotalDelay;              // 上一周期记录的总时延
        Time     lastReportTime;              // 上一次发送报告的时间

        // 从RTP头中读取的、摄像头已发送的总包数
        uint32_t lastReportedSentPackets;     // 上一次从RTP头读取到的发送总数
        uint32_t maxSeenSentPackets;          // 当前周期看到的最大已发送包数
        
        EventId reportEvent;         // 统计报告事件ID
        EventId playEvent;           // 播放事件ID

        // 播放器相关
        uint32_t playFrameSeq;       // 当前期望播放的帧序号
        uint32_t playFrameRate;      // 播放帧率

        // 构造函数，初始化所有变量
        ClientSession() : 
            currentTotalReceivedPackets(0),
            currentTotalReceivedBytes(0),
            currentTotalDelay(Seconds(0)),
            lastTotalReceivedPackets(0),
            lastTotalReceivedBytes(0),
            lastTotalDelay(Seconds(0)),
            lastReportTime(Seconds(0)),
            lastReportedSentPackets(0),
            maxSeenSentPackets(0),
            playFrameSeq(0),
            playFrameRate(30)
        {
            // 构造函数体为空
        }
    };

    void HandleRead(Ptr<Socket> socket); // 处理接收到的数据包
    void ProcessRtp(Ptr<Packet> packet, const Address& from); // 处理RTP包
    void ProcessRtsp(Ptr<Packet> packet, const Address& from); // 处理RTSP包
    void ScheduleReport(const Address& clientAddress); // 调度统计报告
    void SendRtcpFeedback(const Address& clientAddress); // 发送RTCP反馈
    void Playback(const Address& clientAddress);      // 模拟播放

    Ptr<Socket> m_socket;      // 服务器的Socket
    uint16_t m_port;           // 监听的端口
    Time m_reportInterval;     // 统计报告的间隔

    // 存储每个客户端会话的map，key是客户端地址
    std::map<Address, ClientSession> m_sessions;
};

} // namespace ns3

#endif /* YTY_SERVER_H */