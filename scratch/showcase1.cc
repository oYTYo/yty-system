/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * mixed_codec_fairness.cc
 *
 * 场景描述: 验证一下GCC到底是不是均分
 * 验证不同 Codec 和不同应用层限速下的带宽公平性。
 * 使用 scratch/bandwidth.txt 中的真实带宽轨迹驱动。
 *
 * 拓扑:
 * Cam1 (H.264, Cap 5M) ---+
 * |
 * Switch ----(Bottleneck)---- Server
 * |
 * Cam2 (AV1,   Cap 3M) ---+
 *
 * 流程:
 * 0-10s: 动态乘数为 5.0。预期总带宽 ~10Mbps (Cam1->5M, Cam2->3M)。
 * 10-20s: 动态乘数降为 2.0。预期总带宽 ~4Mbps。GCC 应保证两者均分带宽 (各~2M)。
 */

#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/applications-module.h"
#include "ns3/point-to-point-module.h"
#include <fstream>
#include <vector>

// 引入我们的模块头文件
#include "yty-camera-helper.h"
#include "yty-server-helper.h"
#include "yty-camera.h"
#include "yty-server.h"

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("MixedCodecFairness");

// -------------------------------------------------------------------------
// 动态带宽调整逻辑 (移植自 wired_network.cc)
// -------------------------------------------------------------------------

/**
 * @brief 修改设备带宽
 */
void ChangeBandwidth(NetDeviceContainer devices, DataRate newBandwidth)
{
    for (uint32_t i = 0; i < devices.GetN(); ++i)
    {
        Ptr<PointToPointNetDevice> p2pDevice = DynamicCast<PointToPointNetDevice>(devices.Get(i));
        if (p2pDevice)
        {
            p2pDevice->SetDataRate(newBandwidth);
        }
    }
    // NS_LOG_UNCOND("At time " << Simulator::Now().GetSeconds() << "s, Bottleneck bandwidth updated to " << newBandwidth);
}

/**
 * @brief 周期性调度带宽变化
 */
void ScheduleNextBandwidthChange(NetDeviceContainer devices, const std::vector<double>& bandwidths_kbps, uint32_t& index)
{
    if (bandwidths_kbps.empty()) return;

    // 循环读取 Trace
    if (index >= bandwidths_kbps.size()) {
        index = 0;
    }

    double currentTime = Simulator::Now().GetSeconds();
    
    // --- 核心控制逻辑 ---
    // 0-10s: Multiplier = 5.0 (目标 ~5Mbps/cam)
    // >10s:  Multiplier = 2.0 (目标 ~2Mbps/cam)
    double dynamic_multiplier = (currentTime < 30.0) ? 5.0 : 2.0;

    // 映射公式: TraceValue * CamCount * 1000 * Multiplier / AvgTraceValue(3.5)
    // 结果单位: bps (因为 * 1000 变成了 bps/kbps转换? 不，原代码 *1000 是为了把 kbps 转为 bps 的一部分，
    // 但这里 new_kbps 变量名是 kbps。让我们仔细核对 wired_network.cc 的逻辑：
    // new_kbps = val * 12 * 1000 * mult / 3.5 -> 这算出来如果是 kbps，那数值非常大。
    // 假设 trace 值是 ~3.5。
    // 3.5 * 12 * 1000 * 1.5 / 3.5 = 18000 (kbps) = 18 Mbps。这是合理的。
    // 所以这里我们也保持一致，计算出的结果单位是 kbps。
    
    double new_kbps = bandwidths_kbps[index] * 2 * 1000 * dynamic_multiplier / 3.5;
    
    DataRate newRate(std::to_string(new_kbps) + "Kbps");
    ChangeBandwidth(devices, newRate);

    index++;
    // 每 1.0 秒更新一次
    Simulator::Schedule(Seconds(1.0), &ScheduleNextBandwidthChange, devices, bandwidths_kbps, index);
}

// -------------------------------------------------------------------------
// 主函数
// -------------------------------------------------------------------------

int main(int argc, char* argv[])
{
    // 开启日志
    LogComponentEnable("MixedCodecFairness", LOG_LEVEL_INFO);

    // 仿真时长
    double simulationTime = 60.0;

    CommandLine cmd;
    cmd.Parse(argc, argv);

    // --- 1. 创建节点 ---
    NodeContainer cameras;
    cameras.Create(2); // Node 0 (H.264), Node 1 (AV1)
    
    NodeContainer switchNode;
    switchNode.Create(1); // Node 2

    NodeContainer serverNode;
    serverNode.Create(1); // Node 3

    // --- 2. 安装协议栈 ---
    InternetStackHelper internet;
    internet.Install(cameras);
    internet.Install(switchNode);
    internet.Install(serverNode);

    // --- 3. 配置链路 ---
    
    // 3.1 摄像头 -> 交换机 (高带宽，无瓶颈)
    PointToPointHelper p2pCam;
    p2pCam.SetDeviceAttribute("DataRate", StringValue("100Mbps"));
    p2pCam.SetChannelAttribute("Delay", StringValue("1ms"));
    p2pCam.SetQueue("ns3::DropTailQueue", "MaxSize", StringValue("100p"));

    NetDeviceContainer cam1Devs = p2pCam.Install(cameras.Get(0), switchNode.Get(0));
    NetDeviceContainer cam2Devs = p2pCam.Install(cameras.Get(1), switchNode.Get(0));

    // 3.2 交换机 -> 服务器 (瓶颈链路)
    // 初始带宽先给个默认值，马上会被 Trace 覆盖
    PointToPointHelper p2pServer;
    p2pServer.SetDeviceAttribute("DataRate", StringValue("10Mbps")); 
    p2pServer.SetChannelAttribute("Delay", StringValue("5ms"));
    p2pServer.SetQueue("ns3::DropTailQueue", "MaxSize", StringValue("100p")); 

    NetDeviceContainer serverDevs = p2pServer.Install(switchNode.Get(0), serverNode.Get(0));

    // --- 4. 分配 IP ---
    Ipv4AddressHelper ipv4;
    
    // Cam1 子网
    ipv4.SetBase("10.1.1.0", "255.255.255.0");
    Ipv4InterfaceContainer cam1Ifaces = ipv4.Assign(cam1Devs);
    
    // Cam2 子网
    ipv4.SetBase("10.1.2.0", "255.255.255.0");
    Ipv4InterfaceContainer cam2Ifaces = ipv4.Assign(cam2Devs);
    
    // Server 子网
    ipv4.SetBase("10.1.3.0", "255.255.255.0");
    Ipv4InterfaceContainer serverIfaces = ipv4.Assign(serverDevs);

    Ipv4Address serverIp = serverIfaces.GetAddress(1);
    Ipv4GlobalRoutingHelper::PopulateRoutingTables();

    // --- 5. 安装应用 ---

    uint16_t port = 9;

    // 5.1 安装 Server
    YtyServerHelper serverHelper(port);
    serverHelper.SetAttribute("LogFile", StringValue("scratch/fairness_test_server_log.txt"));
    serverHelper.SetAttribute("UseAI", BooleanValue(false));
    serverHelper.SetAttribute("UseMinerva", BooleanValue(false));
    serverHelper.SetAttribute("UseOracle", BooleanValue(false));
    ApplicationContainer serverApp = serverHelper.Install(serverNode.Get(0));
    serverApp.Start(Seconds(0.5));
    serverApp.Stop(Seconds(simulationTime));
    
    // 注册客户端信息
    Ptr<YtyServer> serverPtr = serverApp.Get(0)->GetObject<YtyServer>();
    serverPtr->RegisterClientInfo(cam1Ifaces.GetAddress(0), YtyServer::ClientInfo(1, "wired", "ZoneA", "H.264"));
    serverPtr->RegisterClientInfo(cam2Ifaces.GetAddress(0), YtyServer::ClientInfo(2, "wired", "ZoneA", "AV1"));

    // 5.2 安装 Camera 1 (H.264, Cap 5Mbps)
    YtyCameraHelper cam1Helper(serverIp, port);
    cam1Helper.SetAttribute("CameraId", UintegerValue(1));
    cam1Helper.SetAttribute("Codec", StringValue("H.264"));
    cam1Helper.SetAttribute("MaxAppBitrate", DataRateValue(DataRate("5Mbps"))); // 应用层限速
    
    ApplicationContainer cam1App = cam1Helper.Install(cameras.Get(0));
    cam1App.Start(Seconds(1.0));
    cam1App.Stop(Seconds(simulationTime));

    // 5.3 安装 Camera 2 (AV1, Cap 3Mbps)
    YtyCameraHelper cam2Helper(serverIp, port);
    cam2Helper.SetAttribute("CameraId", UintegerValue(2));
    cam2Helper.SetAttribute("Codec", StringValue("AV1"));
    cam2Helper.SetAttribute("MaxAppBitrate", DataRateValue(DataRate("3Mbps"))); // 应用层限速

    ApplicationContainer cam2App = cam2Helper.Install(cameras.Get(1));
    cam2App.Start(Seconds(1.0));
    cam2App.Stop(Seconds(simulationTime));

    // --- 6. 读取 Bandwidth.txt 并调度 ---
    std::vector<double> bandwidthScheduleKbps;
    std::ifstream bandwidthFile("scratch/bandwidth.txt"); 

    if (bandwidthFile.is_open())
    {
        double kbps;
        while (bandwidthFile >> kbps)
        {
            bandwidthScheduleKbps.push_back(kbps);
        }
        bandwidthFile.close();
        NS_LOG_INFO("Successfully loaded bandwidth schedule from scratch/bandwidth.txt");
    }
    else
    {
        NS_LOG_WARN("Could not open scratch/bandwidth.txt. Using static bandwidth.");
    }

    if (!bandwidthScheduleKbps.empty())
    {
        // 从第0行开始读取，或者你可以根据 wired_network.cc 设置为 15000
        static uint32_t bandwidthIndex = 0; 
        
        // 立即调度第一次带宽更新
        Simulator::ScheduleNow(&ScheduleNextBandwidthChange, serverDevs, bandwidthScheduleKbps, bandwidthIndex);
    }
    else
    {
        // 如果文件读取失败，回退到硬编码的简单调度
        Simulator::Schedule(Seconds(30.0), &ChangeBandwidth, serverDevs, DataRate("4Mbps"));
    }

    // --- 7. 运行仿真 ---
    NS_LOG_UNCOND("Starting Simulation with Dynamic Bandwidth Trace...");
    NS_LOG_UNCOND("0-10s: Multiplier 5.0 (Trace driven, ~10Mbps total).");
    NS_LOG_UNCOND("10-20s: Multiplier 2.0 (Trace driven, ~4Mbps total).");
    
    Simulator::Stop(Seconds(simulationTime + 1.0));
    Simulator::Run();
    Simulator::Destroy();
    
    NS_LOG_UNCOND("Simulation Finished.");

    return 0;
}