/*
 * 1000cameras.cc
 *
 * 网络描述:
 * 这是一个超大规模的摄像头监控网络仿真脚本，包含1000个摄像头。
 * 网络构成为：60% 有线 (600个)，20% Wi-Fi (200个)，20% LTE (200个)。
 * * 拓扑结构 (10个区域, 三层树状拓扑):
 * - 服务器 (Server) 
 * └── 核心路由器 (Core Router)  <-- [10Gbps 高速无阻塞链路]
 * ├── LTE核心网网关 (PGW)       <-- 【瓶颈链路 A：受动态带宽控制，承载200路LTE】
 * └── 汇聚路由器 x 10           <-- 【瓶颈链路 B：受动态带宽控制，每条承载80路有线/WiFi】
 * ├── (每个区域) 有线交换机 x 6  -> 每个交换机挂载 10个 有线摄像头
 * ├── (每个区域) Wi-Fi AP x 2   -> 每个AP挂载 10个 Wi-Fi摄像头 (STA)
 * └── (每个区域) eNB基站 x 2    -> 每个基站挂载 10个 LTE摄像头 (UE)
 */

#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/mobility-module.h"
#include "ns3/lte-module.h"
#include "ns3/applications-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/wifi-module.h"
#include "ns3/csma-module.h"

#include "ns3/yty-camera-helper.h"
#include "ns3/yty-server-helper.h"
#include "ns3/yty-camera.h"
#include "ns3/yty-server.h"

#include <cmath>
#include <fstream>
#include <vector>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("OneThousandCameraNetwork");

// ===================================================================================
// --- 动态带宽调整函数 ---
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
}

// 接收两组设备：区域汇聚链路和 PGW 链路
void ScheduleNextBandwidthChange(NetDeviceContainer aggDevs, NetDeviceContainer pgwDevs, const std::vector<double>& bandwidths_kbps, uint32_t& index, double simulationTime, double baseMultiplier, Ptr<YtyServer> serverApp, bool useOracle)
{
    if (index >= bandwidths_kbps.size()) {
        NS_LOG_INFO("Bandwidth schedule finished, restarting from the beginning.");
        index = 0;
    }

    double dynamic_multiplier = baseMultiplier;

    // 1. 计算单个区域汇聚链路的带宽 (每个区域 80 个摄像头)
    double agg_kbps = bandwidths_kbps[index] * 80 * 1000 * dynamic_multiplier / 3.5;
    DataRate aggRate(std::to_string(agg_kbps) + "Kbps");
    ChangeBandwidth(aggDevs, aggRate);

    // 2. 计算 PGW 链路的带宽 (统一承载全网 200 个LTE摄像头)
    double pgw_kbps = bandwidths_kbps[index] * 200 * 1000 * dynamic_multiplier / 3.5;
    DataRate pgwRate(std::to_string(pgw_kbps) + "Kbps");
    ChangeBandwidth(pgwDevs, pgwRate);

    NS_LOG_UNCOND("At time " << Simulator::Now().GetSeconds() << "s, Aggregation Link Bandwidth (x10): " << aggRate << ", PGW Link Bandwidth (x1): " << pgwRate);

    // 如果启用了 Oracle，计算总带宽反馈给服务器
    if (useOracle && serverApp)
    {
        double total_kbps = agg_kbps * 10 + pgw_kbps;
        serverApp->SetTotalBandwidth(DataRate(std::to_string(total_kbps) + "Kbps"));
    }

    index++;
    Simulator::Schedule(Seconds(1.0), &ScheduleNextBandwidthChange, aggDevs, pgwDevs, bandwidths_kbps, index, simulationTime, baseMultiplier, serverApp, useOracle);
}


int main(int argc, char* argv[])
{
    double simulationTime = 360.0;
    double baseMultiplier = 1.5;
    uint32_t startLine = 15000;

    bool useAI = false;
    bool useMinerva = false;
    bool useOracle = false;
    bool useUniQ = true;
    uint32_t traceCameraId = 1; 
    std::string targetCodec = "Mixed";

    CommandLine cmd;
    cmd.AddValue("useAI", "Enable AI-based congestion control", useAI);
    cmd.AddValue("useMinerva", "Enable Minerva-like QoE-based rate adjustment", useMinerva);
    cmd.AddValue("useUniQ", "Enable UniQ algorithm via ZMQ", useUniQ);
    cmd.AddValue("useOracle", "Enable Oracle absolute bandwidth feedback", useOracle);
    cmd.AddValue("traceCameraId", "ID of the camera to trace", traceCameraId);
    cmd.AddValue("targetCodec", "Force codec (AV1, VP9, H.265, H.264, or Mixed)", targetCodec);
    cmd.AddValue("time", "Total simulation time", simulationTime);
    cmd.AddValue("base", "Base multiplier for dynamic bandwidth", baseMultiplier);
    cmd.AddValue("startLine", "Line number to start reading bandwidth from", startLine);
    cmd.Parse(argc, argv);

    // 网络规模
    const uint32_t REGION_COUNT              = 10;   
    const uint32_t WIRED_CAM_PER_REGION      = 60;   
    const uint32_t WIFI_CAM_PER_REGION       = 20;   
    const uint32_t LTE_CAM_PER_REGION        = 20;   

    const uint32_t CAM_PER_SWITCH            = 10;
    const uint32_t WIRED_SWITCH_PER_REGION   = WIRED_CAM_PER_REGION / CAM_PER_SWITCH;

    const uint32_t CAM_PER_AP                = 10;
    const uint32_t WIFI_AP_PER_REGION        = WIFI_CAM_PER_REGION / CAM_PER_AP;

    const uint32_t CAM_PER_ENB               = 10;
    const uint32_t LTE_ENB_PER_REGION        = LTE_CAM_PER_REGION / CAM_PER_ENB;

    const uint16_t serverPort                = 9;

    NS_LOG_INFO("Configuring LTE Helpers...");
    Ptr<PointToPointEpcHelper> epcHelper = CreateObject<PointToPointEpcHelper>();
    Ptr<LteHelper> lteHelper = CreateObject<LteHelper>();
    lteHelper->SetEpcHelper(epcHelper);
    lteHelper->SetEnbDeviceAttribute("DlBandwidth", UintegerValue(100));
    lteHelper->SetEnbDeviceAttribute("UlBandwidth", UintegerValue(100));

    NS_LOG_INFO("Creating network nodes...");
    NodeContainer serverNode, coreRouterNode;
    serverNode.Create(1);
    coreRouterNode.Create(1);
    Ptr<Node> pgw = epcHelper->GetPgwNode();

    NodeContainer aggRouterNodes;
    aggRouterNodes.Create(REGION_COUNT);

    NodeContainer wiredSwitchNodes[REGION_COUNT];
    NodeContainer wifiApNodes[REGION_COUNT];
    NodeContainer enbNodes[REGION_COUNT];
    NodeContainer wiredCameraNodes[REGION_COUNT];
    NodeContainer wifiStaNodes[REGION_COUNT];
    NodeContainer lteUeNodes[REGION_COUNT];

    for (uint32_t i = 0; i < REGION_COUNT; ++i)
    {
        wiredSwitchNodes[i].Create(WIRED_SWITCH_PER_REGION);
        wifiApNodes[i].Create(WIFI_AP_PER_REGION);
        enbNodes[i].Create(LTE_ENB_PER_REGION);
        wiredCameraNodes[i].Create(WIRED_CAM_PER_REGION);
        wifiStaNodes[i].Create(WIFI_CAM_PER_REGION);
        lteUeNodes[i].Create(LTE_CAM_PER_REGION);
    }

    NS_LOG_INFO("Configuring Mobility...");
    MobilityHelper mobility;
    mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");

    Ptr<ListPositionAllocator> centerAlloc = CreateObject<ListPositionAllocator>();
    centerAlloc->Add(Vector(0.0, 0.0, 0.0));     
    centerAlloc->Add(Vector(10.0, 0.0, 0.0));    
    centerAlloc->Add(Vector(10.0, 10.0, 0.0));   
    mobility.SetPositionAllocator(centerAlloc);
    mobility.Install(serverNode);
    mobility.Install(coreRouterNode);
    mobility.Install(pgw);

    const double R_REGION = 500.0;
    for (uint32_t i = 0; i < REGION_COUNT; ++i)
    {
        double angle = 2 * M_PI * i / REGION_COUNT;
        Vector aggPos(R_REGION * std::cos(angle), R_REGION * std::sin(angle), 0.0);
        
        Ptr<ListPositionAllocator> aggAlloc = CreateObject<ListPositionAllocator>();
        aggAlloc->Add(aggPos);
        mobility.SetPositionAllocator(aggAlloc);
        mobility.Install(aggRouterNodes.Get(i));

        Ptr<RandomDiscPositionAllocator> discAlloc = CreateObject<RandomDiscPositionAllocator>();
        discAlloc->SetX(aggPos.x);
        discAlloc->SetY(aggPos.y);
        discAlloc->SetRho(CreateObjectWithAttributes<UniformRandomVariable>("Min", DoubleValue(20.0), "Max", DoubleValue(150.0)));
        mobility.SetPositionAllocator(discAlloc);
        
        mobility.Install(wiredSwitchNodes[i]);
        mobility.Install(wifiApNodes[i]);
        // 注意：基站和LTE终端的分配被移到了下面
        mobility.Install(wiredCameraNodes[i]);
        mobility.Install(wifiStaNodes[i]);

        // === 修改部分：LTE 基站与摄像机的精细化聚拢分布 ===
        for (uint32_t j = 0; j < LTE_ENB_PER_REGION; ++j) {
            // 将基站分布在汇聚路由周围
            Vector enbPos(aggPos.x + 80 * std::cos(j * M_PI), aggPos.y + 80 * std::sin(j * M_PI), 0.0);
            Ptr<ListPositionAllocator> enbAlloc = CreateObject<ListPositionAllocator>();
            enbAlloc->Add(enbPos);
            MobilityHelper enbMobility;
            enbMobility.SetPositionAllocator(enbAlloc);
            enbMobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
            enbMobility.Install(enbNodes[i].Get(j));

            // 将这 10 个 LTE 摄像机严格限制在所属基站周围 5~50 米范围内
            Ptr<RandomDiscPositionAllocator> ueAlloc = CreateObject<RandomDiscPositionAllocator>();
            ueAlloc->SetX(enbPos.x);
            ueAlloc->SetY(enbPos.y);
            ueAlloc->SetRho(CreateObjectWithAttributes<UniformRandomVariable>("Min", DoubleValue(5.0), "Max", DoubleValue(50.0)));
            MobilityHelper ueMobility;
            ueMobility.SetPositionAllocator(ueAlloc);
            ueMobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
            
            NodeContainer theseUes;
            for (uint32_t k = 0; k < CAM_PER_ENB; ++k) {
                theseUes.Add(lteUeNodes[i].Get(j * CAM_PER_ENB + k));
            }
            ueMobility.Install(theseUes);
        }
    }

    NS_LOG_INFO("Installing IP protocol stack...");
    InternetStackHelper internet;
    internet.Install(serverNode);
    internet.Install(coreRouterNode);
    internet.Install(aggRouterNodes);
    
    for (uint32_t i = 0; i < REGION_COUNT; ++i)
    {
        internet.Install(wiredSwitchNodes[i]);
        internet.Install(wifiApNodes[i]);
        internet.Install(enbNodes[i]);
        internet.Install(wiredCameraNodes[i]);
        internet.Install(wifiStaNodes[i]);
        internet.Install(lteUeNodes[i]);
    }

    NS_LOG_INFO("Configuring Links and Subnets...");
    Ipv4AddressHelper ipv4h;
    Ipv4StaticRoutingHelper ipv4RoutingHelper;

    // 服务器到核心路由的链路：变更为固定高带宽，不作为瓶颈
    PointToPointHelper p2pCoreToServer;
    p2pCoreToServer.SetDeviceAttribute("DataRate", StringValue("10Gbps"));
    p2pCoreToServer.SetChannelAttribute("Delay", StringValue("2ms"));
    
    NetDeviceContainer coreToServerDevs = p2pCoreToServer.Install(coreRouterNode.Get(0), serverNode.Get(0));
    ipv4h.SetBase("10.255.255.0", "255.255.255.252");
    Ipv4InterfaceContainer coreToServerIfaces = ipv4h.Assign(coreToServerDevs);
    Ipv4Address serverIp = coreToServerIfaces.GetAddress(1);
    ipv4RoutingHelper.GetStaticRouting(serverNode.Get(0)->GetObject<Ipv4>())->SetDefaultRoute(coreToServerIfaces.GetAddress(0), 1);

    // 用于瓶颈链路的 P2P 助手 (增加缓冲区长度应对拥塞)
    PointToPointHelper p2pBottleneck;
    p2pBottleneck.SetDeviceAttribute("DataRate", StringValue("1Gbps")); // 初始带宽
    p2pBottleneck.SetChannelAttribute("Delay", StringValue("2ms"));
    p2pBottleneck.SetQueue("ns3::DropTailQueue", "MaxSize", StringValue("150p"));

    // 【新增】容器：收集所有需要控制动态带宽的设备
    NetDeviceContainer allCoreToAggDevs; 
    
    // Core Router <-> PGW (瓶颈 A)
    NetDeviceContainer coreToPgwDevs = p2pBottleneck.Install(coreRouterNode.Get(0), pgw);
    ipv4h.SetBase("10.254.254.0", "255.255.255.252");
    Ipv4InterfaceContainer coreToPgwIfaces = ipv4h.Assign(coreToPgwDevs);
    
    Ptr<Ipv4StaticRouting> coreRouting = ipv4RoutingHelper.GetStaticRouting(coreRouterNode.Get(0)->GetObject<Ipv4>());
    uint32_t ifaceCoreToPgw = coreRouterNode.Get(0)->GetObject<Ipv4>()->GetInterfaceForDevice(coreToPgwDevs.Get(0));
    coreRouting->AddNetworkRouteTo(Ipv4Address("7.0.0.0"), Ipv4Mask("255.0.0.0"), coreToPgwIfaces.GetAddress(1), ifaceCoreToPgw);
    
    uint32_t ifacePgwToCore = pgw->GetObject<Ipv4>()->GetInterfaceForDevice(coreToPgwDevs.Get(1));
    ipv4RoutingHelper.GetStaticRouting(pgw->GetObject<Ipv4>())->SetDefaultRoute(coreToPgwIfaces.GetAddress(0), ifacePgwToCore);

    PointToPointHelper p2pAccess;
    p2pAccess.SetDeviceAttribute("DataRate", StringValue("1Gbps"));
    p2pAccess.SetChannelAttribute("Delay", StringValue("1ms"));

    YansWifiChannelHelper wifiChannelHelper = YansWifiChannelHelper::Default();
    Ptr<YansWifiChannel> wifiChannel = wifiChannelHelper.Create();

    // 在进入区域配置循环前，定义一个起始频点用于全局错开干扰
    uint32_t currentEarfcn = 100; 

    for (uint32_t i = 0; i < REGION_COUNT; ++i)
    {
        // Core Router <-> Agg Router (瓶颈 B)
        NetDeviceContainer coreToAggDevs = p2pBottleneck.Install(coreRouterNode.Get(0), aggRouterNodes.Get(i));
        allCoreToAggDevs.Add(coreToAggDevs); // 加入集中控制容器
        
        ipv4h.SetBase(("10.0." + std::to_string(i) + ".0").c_str(), "255.255.255.252");
        Ipv4InterfaceContainer coreToAggIfaces = ipv4h.Assign(coreToAggDevs);
        
        Ptr<Ipv4StaticRouting> aggRouting = ipv4RoutingHelper.GetStaticRouting(aggRouterNodes.Get(i)->GetObject<Ipv4>());
        aggRouting->SetDefaultRoute(coreToAggIfaces.GetAddress(0), 1);
        
        uint32_t ifaceCoreToAgg = coreRouterNode.Get(0)->GetObject<Ipv4>()->GetInterfaceForDevice(coreToAggDevs.Get(0));

        // 有线网络接入
        for (uint32_t j = 0; j < WIRED_SWITCH_PER_REGION; ++j)
        {
            NetDeviceContainer aggToSwitchDevs = p2pAccess.Install(aggRouterNodes.Get(i), wiredSwitchNodes[i].Get(j));
            ipv4h.SetBase(("10.1." + std::to_string(i) + "." + std::to_string(j * 4)).c_str(), "255.255.255.252");
            Ipv4InterfaceContainer aggToSwitchIfaces = ipv4h.Assign(aggToSwitchDevs);
            
            ipv4RoutingHelper.GetStaticRouting(wiredSwitchNodes[i].Get(j)->GetObject<Ipv4>())->SetDefaultRoute(aggToSwitchIfaces.GetAddress(0), 1);
            
            uint32_t ifaceAggToSwitch = aggRouterNodes.Get(i)->GetObject<Ipv4>()->GetInterfaceForDevice(aggToSwitchDevs.Get(0));
            std::string subnetStr = "192.168." + std::to_string(i * 10 + j) + ".0";
            aggRouting->AddNetworkRouteTo(Ipv4Address(subnetStr.c_str()), Ipv4Mask("255.255.255.0"), aggToSwitchIfaces.GetAddress(1), ifaceAggToSwitch);
            coreRouting->AddNetworkRouteTo(Ipv4Address(subnetStr.c_str()), Ipv4Mask("255.255.255.0"), coreToAggIfaces.GetAddress(1), ifaceCoreToAgg);

            for (uint32_t k = 0; k < CAM_PER_SWITCH; ++k)
            {
                Ptr<Node> camNode = wiredCameraNodes[i].Get(j * CAM_PER_SWITCH + k);
                NetDeviceContainer camToSwitchDevs = p2pAccess.Install(camNode, wiredSwitchNodes[i].Get(j));
                std::string ipBase = "192.168." + std::to_string(i * 10 + j) + "." + std::to_string(k * 4);
                ipv4h.SetBase(ipBase.c_str(), "255.255.255.252");
                Ipv4InterfaceContainer camIfaces = ipv4h.Assign(camToSwitchDevs);
                ipv4RoutingHelper.GetStaticRouting(camNode->GetObject<Ipv4>())->SetDefaultRoute(camIfaces.GetAddress(1), 1);
            }
        }

        // Wi-Fi接入
        for (uint32_t j = 0; j < WIFI_AP_PER_REGION; ++j)
        {
            NetDeviceContainer aggToApDevs = p2pAccess.Install(aggRouterNodes.Get(i), wifiApNodes[i].Get(j));
            ipv4h.SetBase(("10.2." + std::to_string(i) + "." + std::to_string(j * 4)).c_str(), "255.255.255.252");
            Ipv4InterfaceContainer aggToApIfaces = ipv4h.Assign(aggToApDevs);
            ipv4RoutingHelper.GetStaticRouting(wifiApNodes[i].Get(j)->GetObject<Ipv4>())->SetDefaultRoute(aggToApIfaces.GetAddress(0), 1);

            uint32_t ifaceAggToAp = aggRouterNodes.Get(i)->GetObject<Ipv4>()->GetInterfaceForDevice(aggToApDevs.Get(0));
            std::string subnetStr = "192.168." + std::to_string(100 + i * 10 + j) + ".0";
            aggRouting->AddNetworkRouteTo(Ipv4Address(subnetStr.c_str()), Ipv4Mask("255.255.255.0"), aggToApIfaces.GetAddress(1), ifaceAggToAp);
            coreRouting->AddNetworkRouteTo(Ipv4Address(subnetStr.c_str()), Ipv4Mask("255.255.255.0"), coreToAggIfaces.GetAddress(1), ifaceCoreToAgg);

            YansWifiPhyHelper phyHelper;
            phyHelper.SetChannel(wifiChannel);
            phyHelper.Set("ChannelSettings", StringValue("{42, 0, BAND_5GHZ, 0}"));
            phyHelper.Set("Antennas", UintegerValue(2));
            phyHelper.Set("MaxSupportedTxSpatialStreams", UintegerValue(2));
            phyHelper.Set("MaxSupportedRxSpatialStreams", UintegerValue(2));

            WifiHelper wifi;
            wifi.SetStandard(WIFI_STANDARD_80211n);
            wifi.SetRemoteStationManager("ns3::MinstrelHtWifiManager");

            Ssid ssid = Ssid("wifi-region" + std::to_string(i) + "-ap" + std::to_string(j));
            WifiMacHelper staMac, apMac;
            staMac.SetType("ns3::StaWifiMac", "Ssid", SsidValue(ssid));
            apMac.SetType("ns3::ApWifiMac", "Ssid", SsidValue(ssid));

            NodeContainer stasForThisAp;
            for(uint32_t k = 0; k < CAM_PER_AP; ++k) stasForThisAp.Add(wifiStaNodes[i].Get(j * CAM_PER_AP + k));

            NetDeviceContainer staDevices = wifi.Install(phyHelper, staMac, stasForThisAp);
            NetDeviceContainer apDevices = wifi.Install(phyHelper, apMac, wifiApNodes[i].Get(j));

            ipv4h.SetBase(subnetStr.c_str(), "255.255.255.0");
            ipv4h.Assign(staDevices);
            Ipv4InterfaceContainer apIface = ipv4h.Assign(apDevices);
            
            for(uint32_t k = 0; k < CAM_PER_AP; ++k) {
                ipv4RoutingHelper.GetStaticRouting(stasForThisAp.Get(k)->GetObject<Ipv4>())->SetDefaultRoute(apIface.GetAddress(0), 1);
            }
        }

        // === 修改部分：替换后的 LTE 接入逻辑 ===
        for (uint32_t j = 0; j < LTE_ENB_PER_REGION; ++j)
        {
            // 为每个基站分配不同的频点，错开 200 保证 20MHz 频带完全不重叠，彻底消除同频干扰
            lteHelper->SetEnbDeviceAttribute("DlEarfcn", UintegerValue(currentEarfcn));
            lteHelper->SetEnbDeviceAttribute("UlEarfcn", UintegerValue(currentEarfcn + 18000));
            currentEarfcn += 200; 

            NetDeviceContainer enbDev = lteHelper->InstallEnbDevice(enbNodes[i].Get(j));
            
            // 提取出属于当前基站的 UE 节点
            NodeContainer theseUes;
            for (uint32_t k = 0; k < CAM_PER_ENB; ++k) {
                theseUes.Add(lteUeNodes[i].Get(j * CAM_PER_ENB + k));
            }

            NetDeviceContainer ueDevs = lteHelper->InstallUeDevice(theseUes);
            epcHelper->AssignUeIpv4Address(ueDevs);
            
            for(uint32_t k = 0; k < ueDevs.GetN(); ++k) {
                // 将 UE 明确附着到这唯一的当前基站上
                lteHelper->Attach(ueDevs.Get(k), enbDev.Get(0));
                // 设置默认路由
                ipv4RoutingHelper.GetStaticRouting(theseUes.Get(k)->GetObject<Ipv4>())->SetDefaultRoute(epcHelper->GetUeDefaultGatewayAddress(), 1);
            }
        }
    }

    NS_LOG_INFO("Installing Applications...");
    LogComponentEnable("YtyServerApplication", LOG_LEVEL_INFO);
    
    YtyServerHelper serverHelper(serverPort);
    serverHelper.SetAttribute("LogFile", StringValue("scratch/play_status_1000cameras.txt"));
    serverHelper.SetAttribute("UseAI", BooleanValue(useAI));
    serverHelper.SetAttribute("UseMinerva", BooleanValue(useMinerva));
    serverHelper.SetAttribute("UseOracle", BooleanValue(useOracle));
    serverHelper.SetAttribute("UseUniQ", BooleanValue(useUniQ));
    serverHelper.SetAttribute("TraceCameraId", UintegerValue(traceCameraId));

    ApplicationContainer serverApps = serverHelper.Install(serverNode.Get(0));
    serverApps.Start(Seconds(1.0));
    serverApps.Stop(Seconds(simulationTime - 1.0));
    Ptr<YtyServer> serverApp = serverApps.Get(0)->GetObject<YtyServer>();

    YtyCameraHelper cameraHelper(serverIp, serverPort);
    uint32_t globalCamId = 1;

    // 【核心修改】增加 localIndex 和 毫秒级均匀打散
    auto installCamera = [&](Ptr<Node> camNode, std::string netType, std::string regionName, uint32_t localIndex) {
        std::string codec_type = targetCodec;
        if (targetCodec == "Mixed") {
            if (globalCamId % 4 == 0) codec_type = "H.264";
            else if (globalCamId % 4 == 1) codec_type = "H.265";
            else if (globalCamId % 4 == 2) codec_type = "VP9";
            else codec_type = "AV1";
        }

        cameraHelper.SetAttribute("CameraId", UintegerValue(globalCamId));
        cameraHelper.SetAttribute("Codec", StringValue(codec_type));
        ApplicationContainer apps = cameraHelper.Install(camNode);

        Ptr<Ipv4> ipv4 = camNode->GetObject<Ipv4>();
        Ipv4Address ip = ipv4->GetAddress(1, 0).GetLocal();
        serverApp->RegisterClientInfo(ip, YtyServer::ClientInfo(globalCamId, netType, regionName, codec_type));

        // 完美分散逻辑：
        // localIndex * 1.0 决定它在第几秒启动 (例如 2.0s ~ 11.0s)
        // (globalCamId % 100) * 0.01 将这1秒钟平均切分为 100 份（每份10毫秒）
        // 这样全网没有任何两个摄像机会在同一纳秒启动，彻底消除了微观撞车
        apps.Start(Seconds(2.0 + localIndex * 1.0 + (globalCamId % 100) * 0.01));
        apps.Stop(Seconds(simulationTime - 2.0));
        globalCamId++;
    };

    // 遍历所有区域，按组传入 localIndex (即通过取余计算出的 0~9 序号)
    for (uint32_t i = 0; i < REGION_COUNT; ++i) {
        std::string regionName = "Region-" + std::to_string(i + 1);
        
        for (uint32_t j = 0; j < wiredCameraNodes[i].GetN(); ++j) 
            installCamera(wiredCameraNodes[i].Get(j), "wired", regionName, j % CAM_PER_SWITCH);
            
        for (uint32_t j = 0; j < wifiStaNodes[i].GetN(); ++j) 
            installCamera(wifiStaNodes[i].Get(j), "wifi", regionName, j % CAM_PER_AP);
            
        for (uint32_t j = 0; j < lteUeNodes[i].GetN(); ++j) 
            installCamera(lteUeNodes[i].Get(j), "lte", regionName, j % CAM_PER_ENB);
    }

    // 动态带宽加载调度
    std::vector<double> bandwidthScheduleKbps;
    std::ifstream bandwidthFile("scratch/bandwidth.txt"); 

    if (bandwidthFile.is_open()) {
        double kbps;
        while (bandwidthFile >> kbps) bandwidthScheduleKbps.push_back(kbps);
        bandwidthFile.close();
    }

    if (!bandwidthScheduleKbps.empty()) {
        static uint32_t bandwidthIndex = (startLine > 0 && startLine <= bandwidthScheduleKbps.size()) ? (startLine - 1) : 0;
        Simulator::ScheduleNow(&ScheduleNextBandwidthChange, allCoreToAggDevs, coreToPgwDevs, bandwidthScheduleKbps, bandwidthIndex, simulationTime, baseMultiplier, serverApp, useOracle);
    }

    NS_LOG_INFO("Starting simulation...");
    Simulator::Stop(Seconds(simulationTime));
    Simulator::Run();
    Simulator::Destroy();
    
    return 0;
}