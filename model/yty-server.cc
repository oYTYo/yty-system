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
        .AddAttribute("ReportInterval", "Interval for sending RTCP reports.",
                      TimeValue(MilliSeconds(50)),
                      MakeTimeAccessor(&YtyServer::m_reportInterval),
                      MakeTimeChecker());
    return tid;
}

YtyServer::YtyServer() : m_socket(0), m_port(9) {}
YtyServer::~YtyServer() { m_socket = 0; }

void YtyServer::DoDispose(void)
{
    Application::DoDispose();
}

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
}

void YtyServer::StopApplication(void)
{
    for (auto const& [addr, session] : m_sessions) {
        if(session.reportEvent.IsPending()) {
            Simulator::Cancel(session.reportEvent);
        }
    }
    m_sessions.clear();

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
    if (m_sessions.find(from) == m_sessions.end())
    {
        return; // Ignore RTP from unknown clients
    }
    
    ClientSession& session = m_sessions[from];
    uint32_t packetSize = packet->GetSize();

    RtpHeader rtpHeader;
    packet->RemoveHeader(rtpHeader);

    Time sentTime = NanoSeconds(rtpHeader.GetTimestamp());
    Time now = Simulator::Now();
    Time delay = now - sentTime;
    
    session.intervalReceivedPackets++;
    session.intervalReceivedBytes += packetSize;
    session.intervalTotalDelay += delay;
    
    uint32_t cumulativeSentCount = rtpHeader.GetTotalPackets();
    if (cumulativeSentCount > session.maxSeenSentPackets) {
        session.maxSeenSentPackets = cumulativeSentCount;
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
        NS_LOG_INFO("At time " << Simulator::Now().GetSeconds() << "s, Server received PLAY request from " << InetSocketAddress::ConvertFrom(from).GetIpv4());
        if (m_sessions.find(from) == m_sessions.end())
        {
            m_sessions[from] = ClientSession();
            // ▼▼▼ 【核心修正】: 在会话创建时，就将当前时间设为计时起点！ ▼▼▼
            m_sessions[from].lastReportTime = Simulator::Now();
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

    // 【至关重要】重置周期统计变量，并更新状态
    session.intervalReceivedPackets = 0;
    session.intervalReceivedBytes = 0;
    session.intervalTotalDelay = Seconds(0);
    session.lastReportedSentPackets = session.maxSeenSentPackets;
    session.lastReportTime = now;

    ScheduleReport(clientAddress);
}

} // namespace ns3