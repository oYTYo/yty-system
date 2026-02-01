/*
 * simple-wifi-network.cc
 *
 * 网络描述:
 * 这是一个简化的摄像头监控网络仿真脚本 (Wi-Fi版)。
 * * 拓扑结构:
 * 24个 Wi-Fi 摄像头 (STA) -> 1个无线接入点 (AP) -> (有线P2P瓶颈链路) -> 1个服务器
 *
 * 主要特点:
 * 1. 物理层: 802.11n 5GHz, 80MHz 频宽, MIMO 2x2 (复用自 large-scale 配置).
 * 2. 摄像头配置: 默认 Mixed 模式下包含 H.264/H.265/VP9/AV1 四种编码，各6路（总计24路）。
 * 3. 动态带宽: AP 到服务器之间的骨干链路带宽会根据 'scratch/bandwidth.txt' 动态变化.
 * 4. 移动性: AP位于中心，摄像头分布在半径20米范围内.
 *
 */

#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/applications-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/wifi-module.h"      // 新增: Wi-Fi 模块
#include "ns3/mobility-module.h"  // 新增: 移动性模块 (无线仿真必需)

// 包含自定义的应用助手和应用头文件
#include "yty-camera-helper.h"
#include "yty-server-helper.h"
#include "yty-camera.h"
#include "yty-server.h"

#include <fstream>
#include <vector>

#include <limits>
using namespace ns3;

NS_LOG_COMPONENT_DEFINE("SimpleWifiCameraNetwork");

// ===================================================================================
// --- [功能保留] 动态带宽调整函数 ---
// 保持不变，用于控制 AP -> Server 的有线回传链路
// ===================================================================================

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
    NS_LOG_UNCOND("At time " << Simulator::Now().GetSeconds() << "s, Backhaul link bandwidth changed to " << newBandwidth);
}

void ScheduleNextBandwidthChange(NetDeviceContainer devices, const std::vector<double>& bandwidths_kbps, uint32_t& index, double simulationTime, double baseMultiplier, Ptr<YtyServer> serverApp, bool useOracle)
{
    if (index >= bandwidths_kbps.size()) {
        NS_LOG_INFO("Bandwidth schedule finished, restarting from the beginning.");
        index = 0;
    }

    double currentTime = Simulator::Now().GetSeconds();
    
    // 简单的倍率逻辑
    double dynamic_multiplier = baseMultiplier;

    // 计算新带宽 (针对24个摄像头调整)
    double new_kbps = bandwidths_kbps[index] * 24 * 1000 * dynamic_multiplier / 3.5;
    DataRate newRate(std::to_string(new_kbps) + "Kbps");

    ChangeBandwidth(devices, newRate);

    if (useOracle && serverApp)
    {
        serverApp->SetTotalBandwidth(newRate);
    }

    index++;

    Simulator::Schedule(Seconds(1.0), &ScheduleNextBandwidthChange, devices, bandwidths_kbps, index, simulationTime, baseMultiplier, serverApp, useOracle);
}


int main(int argc, char* argv[])
{
    // 仿真时长
    double simulationTime = 660.0;
    double baseMultiplier = 1.5;
    uint32_t startLine = 15000;

    // --- [功能保留] 命令行参数 ---
    bool useAI = false;
    bool useMinerva = false;
    bool useOracle = false;
    bool useUniQ = false;
    uint32_t traceCameraId = 1; 
    // 超过该 CameraId 后关闭带宽分配算法（变为纯 GCC）。默认 UINT32_MAX 表示全局开启。
    uint32_t algoCameraIdLimit = std::numeric_limits<uint32_t>::max();
    std::string targetCodec = "Mixed";

    CommandLine cmd;
    cmd.AddValue("useAI", "Enable AI-based congestion control", useAI);
    cmd.AddValue("useMinerva", "Enable Minerva-like QoE-based rate adjustment", useMinerva);
    cmd.AddValue("useUniQ", "Enable UniQ algorithm via ZMQ", useUniQ);
    cmd.AddValue("traceCameraId", "ID of the camera to trace", traceCameraId);
    cmd.AddValue("algoCameraIdLimit", "Disable bandwidth allocation algorithm for cameras with CameraId > this limit (e.g., 24=all algo, 12=half, 0=all GCC)", algoCameraIdLimit);
    cmd.AddValue("targetCodec", "Force codec (AV1, VP9, H.265, H.264, or Mixed)", targetCodec);
    cmd.AddValue("time", "Total simulation time", simulationTime);
    cmd.AddValue("base", "Base multiplier for dynamic bandwidth", baseMultiplier);
    cmd.AddValue("startLine", "Line number to start reading bandwidth from", startLine);
    cmd.Parse(argc, argv);

    // --- 仿真核心参数 ---
    const uint32_t WIFI_CAM_TOTAL = 24; // 24个Wi-Fi摄像头
    const uint16_t serverPort = 9;

    // --- 1. 节点创建 ---
    NS_LOG_INFO("Creating nodes...");
    NodeContainer serverNode;
    serverNode.Create(1);

    NodeContainer apNode; // 原来的 switchNode 变成了 AP
    apNode.Create(1);

    NodeContainer cameraNodes; // 这些将成为 STA
    cameraNodes.Create(WIFI_CAM_TOTAL);

    // --- 2. [新增] 移动性模型 (无线网络必须) ---
    // 否则所有节点都在 (0,0,0)，虽然信号最好，但不符合物理规律
    NS_LOG_INFO("Configuring Mobility...");
    MobilityHelper mobility;

    // 2.1 将服务器和 AP 固定在原点 (0,0,0)
    Ptr<ListPositionAllocator> fixedAlloc = CreateObject<ListPositionAllocator>();
    fixedAlloc->Add(Vector(0.0, 0.0, 0.0)); // Server
    fixedAlloc->Add(Vector(0.0, 0.0, 0.0)); // AP
    mobility.SetPositionAllocator(fixedAlloc);
    mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
    mobility.Install(serverNode);
    mobility.Install(apNode);

    // 2.2 将摄像头随机放置在 AP 周围半径 20 米的圆盘内
    Ptr<RandomDiscPositionAllocator> discAlloc = CreateObject<RandomDiscPositionAllocator>();
    discAlloc->SetX(0.0);
    discAlloc->SetY(0.0);
    discAlloc->SetRho(CreateObjectWithAttributes<UniformRandomVariable>("Min", DoubleValue(1.0), "Max", DoubleValue(20.0)));
    
    mobility.SetPositionAllocator(discAlloc);
    // 摄像头静止不动
    mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel"); 
    mobility.Install(cameraNodes);


    // --- 3. 安装 IP 协议栈 ---
    NS_LOG_INFO("Installing IP protocol stack...");
    InternetStackHelper internet;
    internet.Install(serverNode);
    internet.Install(apNode);
    internet.Install(cameraNodes);

    // --- 4. 配置网络链路 (Wi-Fi Access + Wired Backhaul) ---
    NS_LOG_INFO("Configuring network links...");

    // (4.1) [修改] 配置 Wi-Fi 接入 (替代原有的 P2P 接入)
    // 配置参考自 new_large-scale-camera-network.cc 的高性能 5GHz 参数
    YansWifiChannelHelper channelHelper;
    channelHelper.SetPropagationDelay("ns3::ConstantSpeedPropagationDelayModel");
    channelHelper.AddPropagationLoss("ns3::FriisPropagationLossModel"); // 视距传播损耗
    Ptr<YansWifiChannel> wifiChannel = channelHelper.Create();

    YansWifiPhyHelper staPhy, apPhy;
    // 关键: 必须关联同一个信道对象
    staPhy.SetChannel(wifiChannel);
    apPhy.SetChannel(wifiChannel);

    // 设置信道: Channel 42 (5GHz), 80MHz 频宽
    staPhy.Set("ChannelSettings", StringValue("{42, 0, BAND_5GHZ, 0}"));
    apPhy.Set("ChannelSettings", StringValue("{42, 0, BAND_5GHZ, 0}"));

    // 配置 MIMO (2x2) 以支持高吞吐量
    staPhy.Set("Antennas", UintegerValue(2));
    staPhy.Set("MaxSupportedTxSpatialStreams", UintegerValue(2));
    staPhy.Set("MaxSupportedRxSpatialStreams", UintegerValue(2));
    apPhy.Set("Antennas", UintegerValue(2));
    apPhy.Set("MaxSupportedTxSpatialStreams", UintegerValue(2));
    apPhy.Set("MaxSupportedRxSpatialStreams", UintegerValue(2));

    WifiHelper wifi;
    wifi.SetStandard(WIFI_STANDARD_80211ax); // 使用 802.11n (或者 WIFI_STANDARD_80211ac)
    wifi.SetRemoteStationManager("ns3::MinstrelHtWifiManager");

    Ssid ssid = Ssid("simple-wifi-network");
    
    WifiMacHelper staMac, apMac;
    staMac.SetType("ns3::StaWifiMac", "Ssid", SsidValue(ssid), "ActiveProbing", BooleanValue(false));
    apMac.SetType("ns3::ApWifiMac", "Ssid", SsidValue(ssid));

    // 安装设备
    NetDeviceContainer staDevices = wifi.Install(staPhy, staMac, cameraNodes);
    NetDeviceContainer apDevices = wifi.Install(apPhy, apMac, apNode);

    // (4.2) 配置 AP 到 Server 的回传链路 (这是瓶颈链路)
    PointToPointHelper p2pBackhaul;
    p2pBackhaul.SetDeviceAttribute("DataRate", StringValue("100Mbps")); // 初始值，会被动态脚本覆盖
    p2pBackhaul.SetChannelAttribute("Delay", StringValue("5ms"));
    p2pBackhaul.SetQueue("ns3::DropTailQueue", "MaxSize", StringValue("150p"));

    NetDeviceContainer apToServerDevs = p2pBackhaul.Install(apNode.Get(0), serverNode.Get(0));

    // --- 5. 分配 IP 地址 ---
    NS_LOG_INFO("Assigning IP addresses...");
    Ipv4AddressHelper ipv4h;
    Ipv4StaticRoutingHelper ipv4RoutingHelper;

    // (5.1) 有线回传网段 (AP <-> Server)
    ipv4h.SetBase("10.1.1.0", "255.255.255.252");
    Ipv4InterfaceContainer apToServerIfaces = ipv4h.Assign(apToServerDevs);
    Ipv4Address serverIp = apToServerIfaces.GetAddress(1);
    Ipv4Address apIp_uplink = apToServerIfaces.GetAddress(0);

    // (5.2) [修改] Wi-Fi 网段 (AP + All Cameras)
    // 此时所有摄像头在同一个子网，而不是单独的 /30
    ipv4h.SetBase("192.168.1.0", "255.255.255.0");
    Ipv4InterfaceContainer apWifiIface = ipv4h.Assign(apDevices);
    Ipv4InterfaceContainer staWifiIfaces = ipv4h.Assign(staDevices);
    Ipv4Address apIp_wifi = apWifiIface.GetAddress(0);

    // --- 6. 配置路由 ---
    // (6.1) Server 路由
    // Server -> 默认网关指向 AP (以便找到 192.168.1.0/24)
    ipv4RoutingHelper.GetStaticRouting(serverNode.Get(0)->GetObject<Ipv4>())->SetDefaultRoute(apIp_uplink, 1);

    // (6.2) AP 路由
    // AP -> 默认网关指向 Server (用于上行流量)
    // AP 不需要配置去摄像头的路由，因为它们是直连网段 (Connected Route)
    ipv4RoutingHelper.GetStaticRouting(apNode.Get(0)->GetObject<Ipv4>())->SetDefaultRoute(serverIp, 1);

    // (6.3) Camera (STA) 路由
    // Cameras -> 默认网关指向 AP 的 Wi-Fi 接口 IP
    for (uint32_t i = 0; i < WIFI_CAM_TOTAL; ++i)
    {
        ipv4RoutingHelper.GetStaticRouting(cameraNodes.Get(i)->GetObject<Ipv4>())->SetDefaultRoute(apIp_wifi, 1);
    }

    // --- 7. 安装应用程序 ---
    NS_LOG_INFO("Installing applications...");

    // (7.1) 服务器应用
    LogComponentEnable("YtyServerApplication", LOG_LEVEL_INFO);
    YtyServerHelper serverHelper(serverPort);
    serverHelper.SetAttribute("LogFile", StringValue("scratch/play_status_wifi2.txt"));
    serverHelper.SetAttribute("UseAI", BooleanValue(useAI));
    serverHelper.SetAttribute("UseMinerva", BooleanValue(useMinerva));
    serverHelper.SetAttribute("UseOracle", BooleanValue(useOracle));
    serverHelper.SetAttribute("UseUniQ", BooleanValue(useUniQ));
    serverHelper.SetAttribute("TraceCameraId", UintegerValue(traceCameraId));

    serverHelper.SetAttribute("AlgoCameraIdLimit", UintegerValue(algoCameraIdLimit));
    ApplicationContainer serverApps = serverHelper.Install(serverNode.Get(0));
    serverApps.Start(Seconds(1.0));
    serverApps.Stop(Seconds(simulationTime - 1.0));

    Ptr<YtyServer> serverApp = serverApps.Get(0)->GetObject<YtyServer>();

    // (7.2) 摄像头应用
    YtyCameraHelper cameraHelper(serverIp, serverPort);
    uint32_t cameraIdCounter = 1;

    for (uint32_t i = 0; i < WIFI_CAM_TOTAL; ++i)
    {
        Ptr<Node> camNode = cameraNodes.Get(i);
        
        std::string codec_type;
        if (targetCodec != "Mixed") {
            codec_type = targetCodec;
        } else {
            if (i % 4 == 0) codec_type = "H.264";
            else if (i % 4 == 1) codec_type = "H.265";
            else if (i % 4 == 2) codec_type = "VP9";
            else codec_type = "AV1";
        }
        
        cameraHelper.SetAttribute("CameraId", UintegerValue(cameraIdCounter));
        cameraHelper.SetAttribute("Codec", StringValue(codec_type));
        ApplicationContainer apps = cameraHelper.Install(camNode);
        
        Ptr<Ipv4> ipv4 = camNode->GetObject<Ipv4>();
        Ipv4Address ip = ipv4->GetAddress(1, 0).GetLocal();
        
        // [修改] 标记为 "wifi" 客户端
        serverApp->RegisterClientInfo(ip, YtyServer::ClientInfo(cameraIdCounter, "wifi", "Wifi-Region", codec_type));

        apps.Start(Seconds(2.0 + i * 0.2));
        apps.Stop(Seconds(simulationTime - 2.0));
        cameraIdCounter++;
    }

    // --- 8. 配置动态带宽 ---
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
        NS_LOG_INFO("Loaded bandwidth schedule.");
    }
    else
    {
        NS_LOG_ERROR("Could not open bandwidth.txt. Using static bandwidth.");
    }

    if (!bandwidthScheduleKbps.empty())
    {
        static uint32_t bandwidthIndex = (startLine > 0) ? (startLine - 1) : 0;
        // 这里的 apToServerDevs 就是以前的 switchToServerDevs
        Simulator::ScheduleNow(&ScheduleNextBandwidthChange, apToServerDevs, bandwidthScheduleKbps, bandwidthIndex, simulationTime, baseMultiplier, serverApp, useOracle);
    }

    // --- 9. 启动仿真 ---
    NS_LOG_INFO("Starting Wi-Fi simulation...");
    Simulator::Stop(Seconds(simulationTime));
    Simulator::Run();
    Simulator::Destroy();
    NS_LOG_UNCOND("Simulation finished.");

    return 0;
}