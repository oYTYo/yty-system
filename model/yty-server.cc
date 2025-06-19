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
        RtpHeader rtpHeader;
        // Try to peek the header to see if it's RTP
        if (packet->PeekHeader(rtpHeader)) {
            ProcessRtp(packet, from);
        } else {
            ProcessRtsp(packet, from);
        }
    }
}

void YtyServer::ProcessRtsp(Ptr<Packet> packet, const Address& from)
{
    uint8_t buffer[100];
    packet->CopyData(buffer, packet->GetSize());
    buffer[packet->GetSize()] = '\0';
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
            Simulator::Cancel(m_sessions[from].reportEvent);
            Simulator::Cancel(m_sessions[from].playEvent);
            m_sessions.erase(from);
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

    // FIX: Remove the header properly from the packet
    RtpHeader rtpHeader;
    packet->RemoveHeader(rtpHeader);

    Time sentTime = NanoSeconds(rtpHeader.GetTimestamp());
    Time now = Simulator::Now();
    Time delay = now - sentTime;
    
    session.packetsReceived++;
    session.totalDelay += delay;
    // This is still a simplification, a more robust solution would track sent packets per frame
    if(rtpHeader.GetPacketSeq() == rtpHeader.GetTotalPackets() - 1) {
        session.totalPacketsSent += rtpHeader.GetTotalPackets();
    }

    session.recvBuffer[rtpHeader.GetFrameSeq()].push_back(packet);

    NS_LOG_INFO("At time " << now.GetSeconds() << "s, Server received packet " << rtpHeader.GetPacketSeq() 
                << " for frame " << rtpHeader.GetFrameSeq() << " from " << InetSocketAddress::ConvertFrom(from).GetIpv4()
                << ". Delay: " << delay.GetMilliSeconds() << " ms.");
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
    
    uint32_t received_since_last = session.packetsReceived;
    uint32_t sent_since_last = session.totalPacketsSent;

    double throughput = (received_since_last * 1400 * 8) / m_reportInterval.GetSeconds();
    Time avgDelay = (received_since_last > 0) ? session.totalDelay / received_since_last : Seconds(0);
    double lossRate = (sent_since_last > 0) ? 1.0 - (double)received_since_last / sent_since_last : 0.0;

    // ▼▼▼ FIX: Create RTCP payload correctly ▼▼▼
    struct RtcpPayload {
        double throughput;
        int64_t delay_ns;
        double lossRate;
    };
    RtcpPayload payload;
    payload.throughput = throughput;
    payload.delay_ns = avgDelay.GetNanoSeconds();
    payload.lossRate = lossRate;

    Ptr<Packet> rtcpPacket = Create<Packet>(reinterpret_cast<uint8_t*>(&payload), sizeof(payload));
    // ▲▲▲ End of RTCP payload fix ▲▲▲

    m_socket->SendTo(rtcpPacket, 0, clientAddress);

    NS_LOG_INFO("At time " << Simulator::Now().GetSeconds() << "s, Server sent RTCP to " << InetSocketAddress::ConvertFrom(clientAddress).GetIpv4()
                << ": Throughput=" << throughput << " bps, AvgDelay=" << avgDelay.GetMilliSeconds() << " ms, LossRate=" << lossRate);

    session.packetsReceived = 0;
    session.totalPacketsSent = 0;
    session.totalDelay = Seconds(0);

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