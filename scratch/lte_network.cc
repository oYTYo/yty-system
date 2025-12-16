/*
 * simple-lte-network.cc
 *
 * 网络描述:
 * 这是一个简化的摄像头监控网络仿真脚本 (LTE版)。
 * * 拓扑结构:
 * 12个 LTE 摄像头 (UE) -> 1个基站 (eNB) -> EPC (SGW/PGW) -> (有线P2P瓶颈链路) -> 1个服务器
 *
 * 主要特点:
 * 1. 物理层: LTE FDD, 20MHz 带宽 (100 RBs), 确保高吞吐量.
 * 2. 核心网: 使用 PointToPointEpcHelper 模拟完整的 EPC 流程.
 * 3. 摄像头配置: 包含6个H.264编码的摄像头和3个H.265编码的摄像头(混合模式).
 * 4. 动态带宽: PGW (核心网网关) 到服务器之间的骨干链路带宽会根据文件动态变化.
 *
 */

#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/mobility-module.h"
#include "ns3/lte-module.h"       // 新增: LTE 模块
#include "ns3/applications-module.h"
#include "ns3/point-to-point-module.h"

// 包含自定义的应用助手和应用头文件
#include "yty-camera-helper.h"
#include "yty-server-helper.h"
#include "yty-camera.h"
#include "yty-server.h"

#include <fstream>
#include <vector>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("SimpleLteCameraNetwork");

// ===================================================================================
// --- [功能保留] 动态带宽调整函数 ---
// 这里的链路将是 PGW -> Server 的链路
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
    double dynamic_multiplier = baseMultiplier;

    // 计算新带宽 (针对12个摄像头调整)
    double new_kbps = bandwidths_kbps[index] * 12 * 1000 * dynamic_multiplier / 3.5;
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
    std::string targetCodec = "Mixed";

    CommandLine cmd;
    cmd.AddValue("useAI", "Enable AI-based congestion control", useAI);
    cmd.AddValue("useMinerva", "Enable Minerva-like QoE-based rate adjustment", useMinerva);
    cmd.AddValue("useUniQ", "Enable UniQ algorithm via ZMQ", useUniQ);
    cmd.AddValue("traceCameraId", "ID of the camera to trace", traceCameraId);
    cmd.AddValue("targetCodec", "Force codec (AV1, VP9, H.265, H.264, or Mixed)", targetCodec);
    cmd.AddValue("time", "Total simulation time", simulationTime);
    cmd.AddValue("base", "Base multiplier for dynamic bandwidth", baseMultiplier);
    cmd.Parse(argc, argv);

    // --- 仿真核心参数 ---
    const uint32_t LTE_CAM_TOTAL = 12; // 12个UE摄像头
    const uint16_t serverPort = 9;

    // --- 1. LTE Helper 配置 ---
    // 必须在创建节点之前配置 LTE Helper
    NS_LOG_INFO("Configuring LTE Helpers...");
    
    Ptr<PointToPointEpcHelper> epcHelper = CreateObject<PointToPointEpcHelper>();
    Ptr<LteHelper> lteHelper = CreateObject<LteHelper>();
    lteHelper->SetEpcHelper(epcHelper);

    // 配置 LTE 物理层参数
    // 设置下行和上行带宽为 100 RBs (Resource Blocks)，对应 20 MHz 带宽
    // 这对于承载 12 路高清视频是必要的
    lteHelper->SetEnbDeviceAttribute("DlBandwidth", UintegerValue(100));
    lteHelper->SetEnbDeviceAttribute("UlBandwidth", UintegerValue(100));

    // --- 2. 节点创建 ---
    NS_LOG_INFO("Creating nodes...");
    NodeContainer serverNode;
    serverNode.Create(1);

    NodeContainer enbNode; // 基站
    enbNode.Create(1);

    NodeContainer ueNodes; // 摄像头 (User Equipment)
    ueNodes.Create(LTE_CAM_TOTAL);

    // 获取 EPC 中的 PGW 节点 (Packet Data Network Gateway)
    // PGW 是 LTE 网络与外部 Internet (服务器) 连接的网关
    Ptr<Node> pgw = epcHelper->GetPgwNode();

    // --- 3. 移动性模型 ---
    NS_LOG_INFO("Configuring Mobility...");
    MobilityHelper mobility;

    // 3.1 固定 Server, PGW, 和 eNB
    Ptr<ListPositionAllocator> fixedAlloc = CreateObject<ListPositionAllocator>();
    fixedAlloc->Add(Vector(0.0, 0.0, 0.0)); // Server
    fixedAlloc->Add(Vector(0.0, 10.0, 0.0)); // PGW (逻辑位置)
    fixedAlloc->Add(Vector(0.0, 0.0, 0.0)); // eNB (基站位于中心)
    mobility.SetPositionAllocator(fixedAlloc);
    mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
    mobility.Install(serverNode);
    mobility.Install(pgw);
    mobility.Install(enbNode);

    // 3.2 摄像头围绕基站随机分布 (半径 50米)
    Ptr<RandomDiscPositionAllocator> discAlloc = CreateObject<RandomDiscPositionAllocator>();
    discAlloc->SetX(0.0);
    discAlloc->SetY(0.0);
    discAlloc->SetRho(CreateObjectWithAttributes<UniformRandomVariable>("Min", DoubleValue(5.0), "Max", DoubleValue(50.0)));
    
    mobility.SetPositionAllocator(discAlloc);
    mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
    mobility.Install(ueNodes);

    // --- 4. 安装 IP 协议栈 ---
    NS_LOG_INFO("Installing IP protocol stack...");
    InternetStackHelper internet;
    internet.Install(serverNode);
    // 注意: PGW, SGW, MME 的协议栈由 EpcHelper 自动处理或部分安装
    // 但我们需要为 ueNodes 和 enbNode 安装
    internet.Install(ueNodes);
    internet.Install(enbNode);
    // PGW 需要安装 IP 栈以便连接服务器 (如果 EpcHelper 没完全做完，显式调用更安全，虽然通常 EpcHelper 会做)
    // internet.Install(pgw); // EpcHelper通常已经处理了PGW的栈

    // --- 5. LTE 设备安装与网络配置 ---
    NS_LOG_INFO("Installing LTE Devices...");

    // 5.1 安装 eNB 设备
    NetDeviceContainer enbDevs = lteHelper->InstallEnbDevice(enbNode);

    // 5.2 安装 UE 设备
    NetDeviceContainer ueDevs = lteHelper->InstallUeDevice(ueNodes);

    // 5.3 为 UE 分配 IP 地址
    // UE 的 IP 地址由 EPC 管理，通常在 7.0.0.0/8 网段
    Ipv4InterfaceContainer ueIpIfaces = epcHelper->AssignUeIpv4Address(ueDevs);

    // 5.4 将 UE 附着到 eNB
    for (uint32_t i = 0; i < ueNodes.GetN(); ++i)
    {
        lteHelper->Attach(ueDevs.Get(i), enbDevs.Get(0));
    }

    // --- 6. 配置回传链路 (PGW <-> Server) ---
    NS_LOG_INFO("Configuring Backhaul Link (PGW -> Server)...");

    PointToPointHelper p2pBackhaul;
    p2pBackhaul.SetDeviceAttribute("DataRate", StringValue("50Mbps")); // 初始带宽
    p2pBackhaul.SetChannelAttribute("Delay", StringValue("5ms"));
    p2pBackhaul.SetQueue("ns3::DropTailQueue", "MaxSize", StringValue("150p"));

    // 连接 PGW 和 Server
    NetDeviceContainer pgwToServerDevs = p2pBackhaul.Install(pgw, serverNode.Get(0));

    // 分配 IP
    Ipv4AddressHelper ipv4h;
    ipv4h.SetBase("1.0.0.0", "255.255.255.252"); // 公网模拟段
    Ipv4InterfaceContainer pgwToServerIfaces = ipv4h.Assign(pgwToServerDevs);
    Ipv4Address pgwIp = pgwToServerIfaces.GetAddress(0);
    Ipv4Address serverIp = pgwToServerIfaces.GetAddress(1);

    // --- 7. 配置路由 ---
    Ipv4StaticRoutingHelper ipv4RoutingHelper;

    // 7.1 Server 路由
    // Server 需要知道如何到达 7.0.0.0/8 (UE 网段)，下一跳是 PGW
    Ptr<Ipv4StaticRouting> serverRouting = ipv4RoutingHelper.GetStaticRouting(serverNode.Get(0)->GetObject<Ipv4>());
    serverRouting->AddNetworkRouteTo(Ipv4Address("7.0.0.0"), Ipv4Mask("255.0.0.0"), pgwIp, 1);

    // 7.2 PGW 路由
    // PGW 需要默认路由指向 Server (用于将来自 UE 的流量发出去)
    Ptr<Ipv4StaticRouting> pgwRouting = ipv4RoutingHelper.GetStaticRouting(pgw->GetObject<Ipv4>());
    pgwRouting->SetDefaultRoute(serverIp, 2); // 接口索引通常是 2 (1是loopback, 2是p2p) - 具体取决于EPC内部接口数量

    // 7.3 UE 路由
    // UE 的默认路由指向 EPC 网关 (已由 epcHelper->AssignUeIpv4Address 自动配置，这里通常不需要手动设)
    // 但为了保险起见，或者如果使用特定的 Helper 变体：
    for (uint32_t i = 0; i < ueNodes.GetN(); ++i)
    {
        Ptr<Ipv4StaticRouting> ueRouting = ipv4RoutingHelper.GetStaticRouting(ueNodes.Get(i)->GetObject<Ipv4>());
        ueRouting->SetDefaultRoute(epcHelper->GetUeDefaultGatewayAddress(), 1);
    }

    // --- 8. 安装应用程序 ---
    NS_LOG_INFO("Installing applications...");

    // (8.1) 服务器应用
    LogComponentEnable("YtyServerApplication", LOG_LEVEL_INFO);
    YtyServerHelper serverHelper(serverPort);
    serverHelper.SetAttribute("LogFile", StringValue("scratch/play_status_lte.txt"));
    serverHelper.SetAttribute("UseAI", BooleanValue(useAI));
    serverHelper.SetAttribute("UseMinerva", BooleanValue(useMinerva));
    serverHelper.SetAttribute("UseOracle", BooleanValue(useOracle));
    serverHelper.SetAttribute("UseUniQ", BooleanValue(useUniQ));
    serverHelper.SetAttribute("TraceCameraId", UintegerValue(traceCameraId));

    ApplicationContainer serverApps = serverHelper.Install(serverNode.Get(0));
    serverApps.Start(Seconds(1.0));
    serverApps.Stop(Seconds(simulationTime - 1.0));

    Ptr<YtyServer> serverApp = serverApps.Get(0)->GetObject<YtyServer>();

    // (8.2) 摄像头应用
    YtyCameraHelper cameraHelper(serverIp, serverPort);
    uint32_t cameraIdCounter = 1;

    for (uint32_t i = 0; i < LTE_CAM_TOTAL; ++i)
    {
        Ptr<Node> camNode = ueNodes.Get(i);
        
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
        
        // [修改] 标记为 "lte" 客户端
        serverApp->RegisterClientInfo(ip, YtyServer::ClientInfo(cameraIdCounter, "lte", "LTE-Region", codec_type));

        apps.Start(Seconds(2.0 + i * 0.2)); // 稍微错开启动
        apps.Stop(Seconds(simulationTime - 2.0));
        cameraIdCounter++;
    }

    // --- 9. 配置动态带宽 ---
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
        static uint32_t bandwidthIndex = startLine - 1;
        // 这里的 pgwToServerDevs 是我们的瓶颈链路
        Simulator::ScheduleNow(&ScheduleNextBandwidthChange, pgwToServerDevs, bandwidthScheduleKbps, bandwidthIndex, simulationTime, baseMultiplier, serverApp, useOracle);
    }

    // --- 10. 启动仿真 ---
    NS_LOG_INFO("Starting LTE simulation...");
    Simulator::Stop(Seconds(simulationTime));
    Simulator::Run();
    Simulator::Destroy();
    NS_LOG_UNCOND("Simulation finished.");

    return 0;
}