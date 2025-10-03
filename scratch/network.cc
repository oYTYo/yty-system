/*
 * large-scale-test.cc (v5 - Final Realistic LTE Config)
 *
 * 网络描述:
 * 包含3个区域、180个摄像头的大型三层监控网络。
 * 有线摄像头 -> 接入交换机 -> 汇聚路由 -> 核心路由 -> 服务器
 * wifi摄像头 -> AP -> 汇聚路由 -> 核心路由 -> 服务器
 * 蜂窝摄像头 -> 基站 -> 核心网(EPC/PGW) -> 核心路由 -> 服务器
 *
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

#include "ns3/netanim-module.h"
#include "ns3/node-list.h"
#include <cmath>
#include <fstream>
#include <vector>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("LargeScaleCameraNetworkPositionedBeautified");



// devices: 要修改的链路设备容器
// newBandwidth: 新的带宽值
void ChangeBandwidth(NetDeviceContainer devices, DataRate newBandwidth)
{
    // 遍历容器中的所有网络设备
    for (uint32_t i = 0; i < devices.GetN(); ++i)
    {
        // 将通用 NetDevice 转换为 PointToPointNetDevice，因为我们要修改的是P2P链路
        Ptr<PointToPointNetDevice> p2pDevice = DynamicCast<PointToPointNetDevice>(devices.Get(i));
        if (p2pDevice)
        {
            // 调用 SetDataRate 方法来实时更新带宽
            p2pDevice->SetDataRate(newBandwidth);
        }
    }
    // 打印日志，方便调试，确认带宽已修改
    NS_LOG_UNCOND("At time " << Simulator::Now().GetSeconds() << "s, Aggregation link bandwidth changed to " << newBandwidth);
}


// devices: 目标链路设备
// bandwidths_kbps: 从文件中读取的带宽值向量
// index: 当前在向量中的位置（使用引用传递，以便修改）
void ScheduleNextBandwidthChange(NetDeviceContainer devices, const std::vector<double>& bandwidths_kbps, uint32_t& index)
{
    // 检查是否已经读完所有预设的带宽值
    if (index >= bandwidths_kbps.size()) {
        // 如果是，则从头开始循环，实现周而复始的变化
        NS_LOG_INFO("带宽值读完了，继续从头开始。");
        index = 0;
    }

    // 从向量中获取当前的带宽值
    double new_kbps = bandwidths_kbps[index] * 60 * 1000 * 10.0 / 3.5;  // 摄像机总数是60，基础是3.5
    // 构造成ns3的DataRate对象
    DataRate newRate(std::to_string(new_kbps) + "Kbps");

    // 调用前面的函数来实际执行修改
    ChangeBandwidth(devices, newRate);

    // 将索引加一，为下一次调度做准备
    index++;

    // 关键一步：使用Simulator::Schedule安排1秒后再次调用本函数，形成循环
    Simulator::Schedule(Seconds(1.0), &ScheduleNextBandwidthChange, devices, bandwidths_kbps, index);
}


int main(int argc, char* argv[])
{

    // --- AI模式开关 ---
    bool useAI = false;
    bool useMinerva = false; // Minerva 开关
    CommandLine cmd;
    // 添加一个命令行参数 --useAI，可以接受 true 或 false
    cmd.AddValue("useAI", "Enable AI-based congestion control", useAI);
    // 添加 useMinerva 命令行参数
    cmd.AddValue("useMinerva", "Enable Minerva-like QoE-based rate adjustment", useMinerva);
    cmd.Parse(argc, argv);


    // --- 仿真核心参数 ---
    const uint32_t REGION_COUNT              = 1;    // 区域数量
    const uint32_t WIRED_CAM_PER_REGION      = 30;   // 每个区域的有线摄像头数量
    const uint32_t WIFI_CAM_PER_REGION       = 20;   // 每个区域的 Wi-Fi 摄像头数量
    const uint32_t LTE_CAM_PER_REGION        = 10;   // 每个区域的 LTE 摄像头数量

    // 计算每个交换机/接入点/eNB 对应的节点数量
    const uint32_t WIRED_SWITCH_PER_REGION   = WIRED_CAM_PER_REGION / 10;
    const uint32_t WIRED_CAM_PER_SWITCH      = WIRED_CAM_PER_REGION / WIRED_SWITCH_PER_REGION;

    const uint32_t WIFI_AP_PER_REGION        = WIFI_CAM_PER_REGION / 10;
    const uint32_t WIFI_STA_PER_AP           = WIFI_CAM_PER_REGION / WIFI_AP_PER_REGION;

    const uint32_t LTE_ENB_PER_REGION        = LTE_CAM_PER_REGION / 10;
    const uint32_t LTE_UE_PER_ENB            = LTE_CAM_PER_REGION / LTE_ENB_PER_REGION;

    const double   simulationTime            = 660.0; // 仿真总时长（秒）
    const uint16_t serverPort                = 9;    // 服务器应用监听端口

    // ===================================================================================
    // --- 新增: 动态带宽计算 ---
    // ===================================================================================
    const double   avgBitratePerCamMbps      = 10.0;  // 假设每个摄像头的平均码率
    const double   bandwidthMargin           = 1;  // 20%的带宽余量以应对突发流量

    // 有线摄像头到接入交换机网络带宽 (有线摄像头 -> 交换机) 弃用csma了
    uint64_t wiredDatarateBps = static_cast<uint64_t>(WIRED_CAM_PER_SWITCH * avgBitratePerCamMbps * 1e6 * bandwidthMargin);
    std::string wiredDataRateStr = std::to_string(wiredDatarateBps / 1000) + "Kbps";
    
    // 接入层P2P链路带宽 (交换机/AP -> 汇聚路由)
    // 取有线和无线接入中需要带宽较大者
    double accessRequiredMbps = std::max(static_cast<double>(WIRED_CAM_PER_SWITCH), static_cast<double>(WIFI_STA_PER_AP)) * avgBitratePerCamMbps;
    uint64_t p2pAccessDatarateBps = static_cast<uint64_t>(accessRequiredMbps * 1e6 * bandwidthMargin);
    std::string p2pAccessDataRateStr = std::to_string(p2pAccessDatarateBps / 1000) + "Kbps";
    
    // (A) 汇聚层P2P链路带宽 (单个汇聚路由 -> 核心路由)
    // 带宽仅需承载其所在区域的总摄像头流量
    uint32_t totalCamPerRegion = WIRED_CAM_PER_REGION + WIFI_CAM_PER_REGION + LTE_CAM_PER_REGION;
    uint64_t p2pAggToCoreDatarateBps = static_cast<uint64_t>(totalCamPerRegion * avgBitratePerCamMbps * 1e6 * bandwidthMargin);
    std::string p2pAggToCoreDataRateStr = std::to_string(p2pAggToCoreDatarateBps / 1000) + "Kbps";

    // (B) 核心层P2P链路带宽 (核心路由 -> 服务器/PGW)
    // 带宽需要承载所有区域的总摄像头流量 (REGION_COUNT * 单区域流量)
    uint32_t totalCameras = totalCamPerRegion * REGION_COUNT;
    uint64_t p2pCoreToServerDatarateBps = static_cast<uint64_t>(totalCameras * avgBitratePerCamMbps * 1e6 * bandwidthMargin);
    std::string p2pCoreToServerDataRateStr = std::to_string(p2pCoreToServerDatarateBps / 1000) + "Kbps";

    // --- 动态带宽计算结束 ---


    // --- 1. 节点创建 ---
    NS_LOG_INFO("正在创建所有网络节点...");
    NodeContainer serverNode, coreRouterNode;
    serverNode.Create(1);
    coreRouterNode.Create(1);

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

    // LTE EPC 辅助工具和节点创建
    Ptr<PointToPointEpcHelper> epcHelper = CreateObject<PointToPointEpcHelper>();
    Ptr<LteHelper> lteHelper = CreateObject<LteHelper>();
    lteHelper->SetEpcHelper(epcHelper);
    Ptr<Node> pgw = epcHelper->GetPgwNode();


    Ptr<Node> sgw = epcHelper->GetSgwNode(); // 获取SGW节点以便定位
    Ptr<Node> mme = NodeList::GetNode(REGION_COUNT*67+4);  // 其实不知道是不是MME

    // ===================================================================================
    // --- 2. [重构] 安装移动模型并分配所有节点位置 ---
    //
    // 采用分层、结构化的方式设置所有节点位置，确保拓扑清晰且所有节点都被正确定位。
    // ===================================================================================
    NS_LOG_INFO("正在配置结构化的节点位置...");
    MobilityHelper mobility;
    mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");

    // --- 2.1 中心层: 服务器与核心路由器 ---
    Ptr<ListPositionAllocator> centerAlloc = CreateObject<ListPositionAllocator>();
    centerAlloc->Add(Vector(0.0, 0.0, 0.0));   // serverNode
    centerAlloc->Add(Vector(0.0, 0.0, 0.0));   // coreRouterNode
    mobility.SetPositionAllocator(centerAlloc);
    mobility.Install(serverNode);
    mobility.Install(coreRouterNode);

    // --- 2.2 内环层: 汇聚路由器和LTE核心网元 ---
    const double R_INNER = 100.0; // 内环半径
    Ptr<ListPositionAllocator> innerRingAlloc = CreateObject<ListPositionAllocator>();
    // 放置汇聚路由器
    for (uint32_t i = 0; i < REGION_COUNT; ++i)
    {
        double angle = 2 * M_PI * i / (REGION_COUNT + 1); // 留一个位置给EPC
        innerRingAlloc->Add(Vector(R_INNER * std::cos(angle), R_INNER * std::sin(angle), 0.0));
    }
    mobility.SetPositionAllocator(innerRingAlloc);
    mobility.Install(aggRouterNodes);

    // 放置EPC节点 (PGW 和 SGW)，放在内环的最后一个位置
    Ptr<ListPositionAllocator> epcAlloc = CreateObject<ListPositionAllocator>();
    double epcAngle = 2 * M_PI * REGION_COUNT / (REGION_COUNT + 1);
    epcAlloc->Add(Vector(R_INNER * std::cos(epcAngle), R_INNER * std::sin(epcAngle), 0.0)); // PGW
    epcAlloc->Add(Vector(R_INNER * std::cos(epcAngle) + 5, R_INNER * std::sin(epcAngle), 0.0)); // SGW (紧挨着PGW)
    epcAlloc->Add(Vector(R_INNER * std::cos(epcAngle), R_INNER * std::sin(epcAngle) + 5, 0.0)); // MME
    mobility.SetPositionAllocator(epcAlloc);
    mobility.Install(pgw);
    mobility.Install(sgw);
    mobility.Install(mme);

    // --- 2.3 外环集群层: 三个监控区域 ---
    const double R_OUTER_CLUSTER   = 400.0; // 区域中心到总中心的半径
    const double R_ACCESS_NODE_RING = 50.0;  // 区域内接入节点环的半径
    const double R_DEVICE_CLOUD     = 40.0;  // 终端设备云的半径 (围绕接入节点)

    for (uint32_t i = 0; i < REGION_COUNT; ++i)
    {
        // 计算当前区域的中心点
        double regionAngle = 2 * M_PI * i / REGION_COUNT;
        Vector regionCenter(R_OUTER_CLUSTER * std::cos(regionAngle), R_OUTER_CLUSTER * std::sin(regionAngle), 0.0);

        uint32_t accessNodeCount = WIRED_SWITCH_PER_REGION + WIFI_AP_PER_REGION + LTE_ENB_PER_REGION;
        uint32_t accessNodeIndex = 0;

        // (A) 定位区域内的有线网络 (交换机 -> 摄像头)
        for (uint32_t j = 0; j < WIRED_SWITCH_PER_REGION; ++j, ++accessNodeIndex)
        {
            // 定位交换机
            double accessNodeAngle = 2 * M_PI * accessNodeIndex / accessNodeCount;
            Vector switchPos(regionCenter.x + R_ACCESS_NODE_RING * std::cos(accessNodeAngle),
                             regionCenter.y + R_ACCESS_NODE_RING * std::sin(accessNodeAngle), 0.0);
            Ptr<ListPositionAllocator> switchAlloc = CreateObject<ListPositionAllocator>();
            switchAlloc->Add(switchPos);
            mobility.SetPositionAllocator(switchAlloc);
            mobility.Install(wiredSwitchNodes[i].Get(j));

            // 定位连接到该交换机的摄像头
            NodeContainer camerasForThisSwitch;
            for (uint32_t k = 0; k < WIRED_CAM_PER_SWITCH; ++k)
            {
                camerasForThisSwitch.Add(wiredCameraNodes[i].Get(j * WIRED_CAM_PER_SWITCH + k));
            }
            Ptr<RandomDiscPositionAllocator> camAlloc = CreateObject<RandomDiscPositionAllocator>();
            camAlloc->SetX(switchPos.x);
            camAlloc->SetY(switchPos.y);
            camAlloc->SetRho(CreateObjectWithAttributes<UniformRandomVariable>("Min", DoubleValue(0.0), "Max", DoubleValue(R_DEVICE_CLOUD)));
            mobility.SetPositionAllocator(camAlloc);
            mobility.Install(camerasForThisSwitch);
        }

        // (B) 定位区域内的Wi-Fi网络 (AP -> 摄像头)
        for (uint32_t j = 0; j < WIFI_AP_PER_REGION; ++j, ++accessNodeIndex)
        {
            // 定位AP
            double accessNodeAngle = 2 * M_PI * accessNodeIndex / accessNodeCount;
            Vector apPos(regionCenter.x + R_ACCESS_NODE_RING * std::cos(accessNodeAngle),
                         regionCenter.y + R_ACCESS_NODE_RING * std::sin(accessNodeAngle), 0.0);
            Ptr<ListPositionAllocator> apAlloc = CreateObject<ListPositionAllocator>();
            apAlloc->Add(apPos);
            mobility.SetPositionAllocator(apAlloc);
            mobility.Install(wifiApNodes[i].Get(j));

            // 定位连接到该AP的STA摄像头
            NodeContainer stasForThisAp;
            for (uint32_t k = 0; k < WIFI_STA_PER_AP; ++k)
            {
                stasForThisAp.Add(wifiStaNodes[i].Get(j * WIFI_STA_PER_AP + k));
            }
            Ptr<RandomDiscPositionAllocator> staAlloc = CreateObject<RandomDiscPositionAllocator>();
            staAlloc->SetX(apPos.x);
            staAlloc->SetY(apPos.y);
            staAlloc->SetRho(CreateObjectWithAttributes<UniformRandomVariable>("Min", DoubleValue(0.0), "Max", DoubleValue(R_DEVICE_CLOUD)));
            mobility.SetPositionAllocator(staAlloc);
            mobility.Install(stasForThisAp);
        }

        // (C) 定位区域内的LTE网络 (eNB -> 摄像头)
        for (uint32_t j = 0; j < LTE_ENB_PER_REGION; ++j, ++accessNodeIndex)
        {
            // 定位eNB
            double accessNodeAngle = 2 * M_PI * accessNodeIndex / accessNodeCount;
            Vector enbPos(regionCenter.x + R_ACCESS_NODE_RING * std::cos(accessNodeAngle),
                          regionCenter.y + R_ACCESS_NODE_RING * std::sin(accessNodeAngle), 0.0);
            Ptr<ListPositionAllocator> enbAlloc = CreateObject<ListPositionAllocator>();
            enbAlloc->Add(enbPos);
            mobility.SetPositionAllocator(enbAlloc);
            mobility.Install(enbNodes[i].Get(j));

            // 定位连接到该eNB的UE摄像头
            NodeContainer uesForThisEnb;
            for (uint32_t k = 0; k < LTE_UE_PER_ENB; ++k)
            {
                uesForThisEnb.Add(lteUeNodes[i].Get(j * LTE_UE_PER_ENB + k));
            }
            Ptr<RandomDiscPositionAllocator> ueAlloc = CreateObject<RandomDiscPositionAllocator>();
            ueAlloc->SetX(enbPos.x);
            ueAlloc->SetY(enbPos.y);
            ueAlloc->SetRho(CreateObjectWithAttributes<UniformRandomVariable>("Min", DoubleValue(0.0), "Max", DoubleValue(R_DEVICE_CLOUD)));
            mobility.SetPositionAllocator(ueAlloc);
            mobility.Install(uesForThisEnb);
        }
    }
    // --- 位置分配结束 ---


    // --- 3. 安装 IP 协议栈 ---
    NS_LOG_INFO("正在安装IP协议栈...");
    InternetStackHelper internet;
    internet.Install(serverNode);
    internet.Install(coreRouterNode);
    internet.Install(aggRouterNodes);
    for (uint32_t i = 0; i < REGION_COUNT; ++i)
    {
        internet.Install(wiredSwitchNodes[i]);
        internet.Install(wifiApNodes[i]);
        internet.Install(wiredCameraNodes[i]);
        internet.Install(wifiStaNodes[i]);
        internet.Install(enbNodes[i]);
        internet.Install(lteUeNodes[i]);
    }
    internet.Install(pgw); // SGW不需要IP栈，它工作在L2

    // 100 RBs 则对应 20 MHz 的信道带宽
    lteHelper->SetEnbDeviceAttribute("DlBandwidth", UintegerValue(100));
    lteHelper->SetEnbDeviceAttribute("UlBandwidth", UintegerValue(100));

    // --- 4. 配置网络链路和IP地址 ---
    NS_LOG_INFO("正在配置所有网络链路和设备...");

    // 创建两种不同的骨干网P2P助手
    // 1. 用于汇聚层到核心层的链路 (较低带宽)
    PointToPointHelper p2pAggToCore;
    p2pAggToCore.SetDeviceAttribute("DataRate", StringValue(p2pAggToCoreDataRateStr));
    p2pAggToCore.SetChannelAttribute("Delay", StringValue("2ms"));
    p2pAggToCore.SetQueue("ns3::DropTailQueue", "MaxSize", StringValue("100p"));

    // 2. 用于核心层到服务器/PGW的链路 (总带宽，是p2pAggToCore的三倍)
    PointToPointHelper p2pCoreToServer;
    p2pCoreToServer.SetDeviceAttribute("DataRate", StringValue(p2pCoreToServerDataRateStr));
    p2pCoreToServer.SetChannelAttribute("Delay", StringValue("2ms"));
    p2pCoreToServer.SetQueue("ns3::DropTailQueue", "MaxSize", StringValue("150p"));

    PointToPointHelper p2pAccess;  // 接入交换机 -> 汇聚路由 独享
    p2pAccess.SetDeviceAttribute("DataRate", StringValue(p2pAccessDataRateStr));
    p2pAccess.SetChannelAttribute("Delay", StringValue("1ms"));
    p2pAccess.SetQueue("ns3::DropTailQueue", "MaxSize", StringValue("50p"));

    PointToPointHelper p2pCamToSwitch;
    p2pCamToSwitch.SetDeviceAttribute("DataRate", StringValue(wiredDataRateStr));
    p2pCamToSwitch.SetChannelAttribute("Delay", StringValue("100us")); // 交换机内部延迟很小
    p2pCamToSwitch.SetQueue("ns3::DropTailQueue", "MaxSize", StringValue("50p")); // 队列可以适当增大

    Ipv4AddressHelper ipv4h;
    Ipv4StaticRoutingHelper ipv4RoutingHelper;

    // (4.1) 服务器 <-> 核心路由器
    NetDeviceContainer serverToCoreDevs = p2pCoreToServer.Install(serverNode.Get(0), coreRouterNode.Get(0));
    ipv4h.SetBase("10.255.255.0", "255.255.255.252");
    Ipv4InterfaceContainer serverToCoreIfaces = ipv4h.Assign(serverToCoreDevs);
    Ipv4Address serverIp = serverToCoreIfaces.GetAddress(0);
    Ipv4Address coreRouterIp_onServerLink = serverToCoreIfaces.GetAddress(1);
    
    ipv4RoutingHelper.GetStaticRouting(serverNode.Get(0)->GetObject<Ipv4>())->SetDefaultRoute(coreRouterIp_onServerLink, 1);

    // (4.2) 核心路由器 <-> 各汇聚路由器
    Ptr<Ipv4StaticRouting> coreRouting = ipv4RoutingHelper.GetStaticRouting(coreRouterNode.Get(0)->GetObject<Ipv4>());

    // 创建一个容器来收集所有汇聚层到核心层的链路
    NetDeviceContainer allCoreToAggDevs;

    for (uint32_t i = 0; i < REGION_COUNT; ++i)
    {
        NetDeviceContainer coreToAggDevs = p2pAggToCore.Install(coreRouterNode.Get(0), aggRouterNodes.Get(i));

        // 将本次循环创建的链路设备添加到总容器中
        allCoreToAggDevs.Add(coreToAggDevs);

        std::string ipBase = "10.0." + std::to_string(i) + ".0";
        ipv4h.SetBase(ipBase.c_str(), "255.255.255.252");
        Ipv4InterfaceContainer ifaces = ipv4h.Assign(coreToAggDevs);
        
        Ipv4Address coreIp = ifaces.GetAddress(0);
        Ipv4Address aggIp = ifaces.GetAddress(1);

        ipv4RoutingHelper.GetStaticRouting(aggRouterNodes.Get(i)->GetObject<Ipv4>())->SetDefaultRoute(coreIp, 1);
        
        std::string wiredNet = "192.168." + std::to_string(i * 50) + ".0";
        coreRouting->AddNetworkRouteTo(Ipv4Address(wiredNet.c_str()), Ipv4Mask("255.255.240.0"), aggIp, i + 2);
        std::string wifiNet = "192.168." + std::to_string(i * 50 + 30) + ".0";
        coreRouting->AddNetworkRouteTo(Ipv4Address(wifiNet.c_str()), Ipv4Mask("255.255.254.0"), aggIp, i + 2);
    }
    
    // (4.3) 汇聚路由器 <-> 接入层 (有线, Wi-Fi)
    for (uint32_t i = 0; i < REGION_COUNT; ++i)
    {
        Ptr<Ipv4StaticRouting> aggRouting = ipv4RoutingHelper.GetStaticRouting(aggRouterNodes.Get(i)->GetObject<Ipv4>());

        // (4.3.1) 有线网络接入
        // --- [核心修改] 使用点对点链路替换CSMA，以准确模拟交换式网络 ---
        for(uint32_t j = 0; j < WIRED_SWITCH_PER_REGION; ++j)
        {
            // --- 步骤1: 配置汇聚路由到接入交换机的P2P链路 (这部分逻辑不变) ---
            NodeContainer aggToSwitchLink(aggRouterNodes.Get(i), wiredSwitchNodes[i].Get(j));
            NetDeviceContainer aggToSwitchDevs = p2pAccess.Install(aggToSwitchLink);

            // 为这条上行链路分配IP
            ipv4h.SetBase(("10.1." + std::to_string(i) + "." + std::to_string(j * 4)).c_str(), "255.255.255.252");
            Ipv4InterfaceContainer aggToSwitchIfaces = ipv4h.Assign(aggToSwitchDevs);
            // 在交换机上设置默认路由，指向汇聚路由器
            ipv4RoutingHelper.GetStaticRouting(wiredSwitchNodes[i].Get(j)->GetObject<Ipv4>())->SetDefaultRoute(aggToSwitchIfaces.GetAddress(0), 1);

            // --- 步骤2: 为每个摄像头创建到交换机的独立P2P链路 ---
            for (uint32_t k = 0; k < WIRED_CAM_PER_SWITCH; ++k)
            {
                // 获取当前摄像头节点
                Ptr<Node> camNode = wiredCameraNodes[i].Get(j * WIRED_CAM_PER_SWITCH + k);
                
                // 安装摄像头到交换机的P2P链路
                NetDeviceContainer camToSwitchDevs = p2pCamToSwitch.Install(camNode, wiredSwitchNodes[i].Get(j));

                // 为每个P2P链路分配一个独立的 /30 子网
                // 例如：区域i, 交换机j, 摄像头k
                // 192.168.0.4, 192.168.0.8 ...
                std::string ipBase = "192.168." + std::to_string(i * 50 + j) + "." + std::to_string(k * 4);
                ipv4h.SetBase(ipBase.c_str(), "255.255.255.252");
                Ipv4InterfaceContainer camToSwitchIfaces = ipv4h.Assign(camToSwitchDevs);
                
                // 在摄像头上设置默认路由，指向它所连接的交换机
                ipv4RoutingHelper.GetStaticRouting(camNode->GetObject<Ipv4>())->SetDefaultRoute(camToSwitchIfaces.GetAddress(1), 1);
                
                // 在汇聚路由器上，为该摄像头的IP地址添加一条主机路由
                // 指向对应的接入交换机
                aggRouting->AddHostRouteTo(camToSwitchIfaces.GetAddress(0), aggToSwitchIfaces.GetAddress(1), j + 2);
            }
        }

        // (4.3.2) Wi-Fi网络接入
        for(uint32_t j = 0; j < WIFI_AP_PER_REGION; ++j)
        {
            NetDeviceContainer aggToApDevs = p2pAccess.Install(aggRouterNodes.Get(i), wifiApNodes[i].Get(j));
            ipv4h.SetBase(("10.2." + std::to_string(i) + "." + std::to_string(j * 4)).c_str(), "255.255.255.252");
            Ipv4InterfaceContainer aggToApIfaces = ipv4h.Assign(aggToApDevs);
            ipv4RoutingHelper.GetStaticRouting(wifiApNodes[i].Get(j)->GetObject<Ipv4>())->SetDefaultRoute(aggToApIfaces.GetAddress(0), 1);

            // --- 全新、正确的 Wi-Fi PHY 和 Channel 配置 ---
            
            // 1. 创建信道助手和信道对象
            //    这个信道对象将被这个AP和它的所有STA共享
            YansWifiChannelHelper channelHelper;
            channelHelper.SetPropagationDelay("ns3::ConstantSpeedPropagationDelayModel");
            channelHelper.AddPropagationLoss("ns3::FriisPropagationLossModel");
            Ptr<YansWifiChannel> wifiChannel = channelHelper.Create();

            // 2. 为AP和STA分别创建PHY助手，但确保它们配置相同且使用同一个信道对象
            YansWifiPhyHelper staPhy, apPhy;

            // --- 关键扩容步骤 (为两个PHY助手进行完全相同的配置) ---
            // a. 关联到同一个信道
            staPhy.SetChannel(wifiChannel);
            apPhy.SetChannel(wifiChannel);

            // b. 设置信道：选择信道号 42 来获取 80 MHz 带宽。
            staPhy.Set("ChannelSettings", StringValue("{42, 0, BAND_5GHZ, 0}"));
            apPhy.Set("ChannelSettings", StringValue("{42, 0, BAND_5GHZ, 0}"));

            // c. 配置 MIMO
            staPhy.Set("Antennas", UintegerValue(2));
            staPhy.Set("MaxSupportedTxSpatialStreams", UintegerValue(2));
            staPhy.Set("MaxSupportedRxSpatialStreams", UintegerValue(2));
            apPhy.Set("Antennas", UintegerValue(2));
            apPhy.Set("MaxSupportedTxSpatialStreams", UintegerValue(2));
            apPhy.Set("MaxSupportedRxSpatialStreams", UintegerValue(2));
            
            // d. (可选) 增加发射功率
            staPhy.Set("TxPowerStart", DoubleValue(20.0));
            staPhy.Set("TxPowerEnd", DoubleValue(20.0));
            apPhy.Set("TxPowerStart", DoubleValue(20.0));
            apPhy.Set("TxPowerEnd", DoubleValue(20.0));

            // 3. 创建并配置通用的 Wifi 和 MAC 助手
            WifiHelper wifi;
            wifi.SetStandard(WIFI_STANDARD_80211n);
            wifi.SetRemoteStationManager("ns3::MinstrelHtWifiManager");

            Ssid ssid = Ssid("wifi-region" + std::to_string(i) + "-ap" + std::to_string(j));
            
            NodeContainer staNodesForThisAp;
            for(uint32_t k = 0; k < WIFI_STA_PER_AP; ++k)
            {
                staNodesForThisAp.Add(wifiStaNodes[i].Get(j * WIFI_STA_PER_AP + k));
            }

            WifiMacHelper staMac, apMac;
            staMac.SetType("ns3::StaWifiMac", "Ssid", SsidValue(ssid), "ActiveProbing", BooleanValue(false));
            apMac.SetType("ns3::ApWifiMac", "Ssid", SsidValue(ssid));

            // 4. 使用各自独立的PHY助手来安装设备
            NetDeviceContainer staDevices = wifi.Install(staPhy, staMac, staNodesForThisAp);
            NetDeviceContainer apDevices = wifi.Install(apPhy, apMac, wifiApNodes[i].Get(j));

            // if (i == 0 && j == 0)
            // {
            //     // 注意：Pcap文件名现在可以反映出新的配置
            //     apPhy.EnablePcap("scratch/wifi-80mhz-ap-region0-ap0", apDevices.Get(0));
            //     staPhy.EnablePcap("scratch/wifi-80mhz-sta-region0-sta0", staDevices.Get(0));
            // }
            
            std::string ipBase = "192.168." + std::to_string(i * 50 + 30 + j) + ".0";
            ipv4h.SetBase(ipBase.c_str(), "255.255.255.0");
            ipv4h.Assign(staDevices);
            Ipv4InterfaceContainer apInterface = ipv4h.Assign(apDevices);
            
            for(uint32_t k = 0; k < WIFI_STA_PER_AP; ++k)
            {
               ipv4RoutingHelper.GetStaticRouting(staNodesForThisAp.Get(k)->GetObject<Ipv4>())->SetDefaultRoute(apInterface.GetAddress(0), 1);
            }
            aggRouting->AddNetworkRouteTo(Ipv4Address(ipBase.c_str()), Ipv4Mask("255.255.255.0"), aggToApIfaces.GetAddress(1), WIRED_SWITCH_PER_REGION + j + 2);
        }
    }

    // (4.4) LTE网络接入 (统一一个EPC)
    NetDeviceContainer coreToPgwDevs = p2pAggToCore.Install(coreRouterNode.Get(0), pgw);
    ipv4h.SetBase("10.254.254.0", "255.255.255.252");
    Ipv4InterfaceContainer coreToPgwIfaces = ipv4h.Assign(coreToPgwDevs);
    Ipv4Address pgwIpOnCoreLink = coreToPgwIfaces.GetAddress(1);

    uint32_t pgwInterfaceToCore = pgw->GetObject<Ipv4>()->GetInterfaceForDevice(coreToPgwDevs.Get(1));
    ipv4RoutingHelper.GetStaticRouting(pgw->GetObject<Ipv4>())->SetDefaultRoute(coreToPgwIfaces.GetAddress(0), pgwInterfaceToCore);
    
    coreRouting->AddNetworkRouteTo(Ipv4Address("7.0.0.0"), Ipv4Mask("255.0.0.0"), pgwIpOnCoreLink, REGION_COUNT + 2);
    
    for (uint32_t i = 0; i < REGION_COUNT; ++i)
    {
       NetDeviceContainer enbLteDevs = lteHelper->InstallEnbDevice(enbNodes[i]);
       NetDeviceContainer ueLteDevs = lteHelper->InstallUeDevice(lteUeNodes[i]);

       epcHelper->AssignUeIpv4Address(ueLteDevs);
       for(uint32_t j = 0; j < ueLteDevs.GetN(); ++j)
       {
           lteHelper->Attach(ueLteDevs.Get(j), enbLteDevs.Get(j / LTE_UE_PER_ENB));
           ipv4RoutingHelper.GetStaticRouting(lteUeNodes[i].Get(j)->GetObject<Ipv4>())->SetDefaultRoute(epcHelper->GetUeDefaultGatewayAddress(), 1);
       }
    }

    // --- 5. 安装应用程序 ---
    NS_LOG_INFO("正在安装所有摄像头和服务器应用...");

    // (5.1) 服务器应用
    LogComponentEnable("YtyServerApplication", LOG_LEVEL_INFO);
    YtyServerHelper serverHelper(serverPort);
    serverHelper.SetAttribute("LogFile", StringValue("scratch/play_status_large_scale.txt"));
    serverHelper.SetAttribute("UseAI", BooleanValue(useAI));
    serverHelper.SetAttribute("UseMinerva", BooleanValue(useMinerva));

    ApplicationContainer serverApps = serverHelper.Install(serverNode.Get(0));
    serverApps.Start(Seconds(1.0));
    serverApps.Stop(Seconds(simulationTime - 1.0));

    Ptr<YtyServer> serverApp = serverApps.Get(0)->GetObject<YtyServer>();
    NS_ASSERT(serverApp != nullptr);


    // (5.2) 摄像头应用 (安装与注册逻辑)
    YtyCameraHelper cameraHelper(serverIp, serverPort);

    uint32_t cameraIdCounter = 1;
    
    Ptr<UniformRandomVariable> codecSelector = CreateObject<UniformRandomVariable>();
    codecSelector->SetAttribute("Min", DoubleValue(0.0));
    codecSelector->SetAttribute("Max", DoubleValue(1.0));

    for (uint32_t i = 0; i < REGION_COUNT; ++i)
    {
        std::string regionName = "Region" + std::to_string(i + 1);

        // --- 有线摄像头 ---
        for (uint32_t j = 0; j < wiredCameraNodes[i].GetN(); ++j)
        {
            Ptr<Node> camNode = wiredCameraNodes[i].Get(j);
            
            // 动态决定Codec类型
            std::string codec_type;

            if (codecSelector->GetValue() < 1.0/3.0) { // 约 1/3 的概率为 H.265
                codec_type = "H.265";
                
            } else { // 约 2/3 的概率为 H.264
                codec_type = "H.264";
                
            }
            
            cameraHelper.SetAttribute("CameraId", UintegerValue(cameraIdCounter));
            cameraHelper.SetAttribute("Codec", StringValue(codec_type)); // 设置Codec
            ApplicationContainer apps = cameraHelper.Install(camNode);
            
            Ptr<Ipv4> ipv4 = camNode->GetObject<Ipv4>();
            Ipv4Address ip = ipv4->GetAddress(1, 0).GetLocal();
            serverApp->RegisterClientInfo(ip, YtyServer::ClientInfo(cameraIdCounter, "wired", regionName, codec_type));

            apps.Start(Seconds(2.0 + (i * totalCamPerRegion + j) * 0.1));
            apps.Stop(Seconds(simulationTime - 2.0));
            cameraIdCounter++;
        }

        // --- WiFi 摄像头 ---
        for (uint32_t j = 0; j < wifiStaNodes[i].GetN(); ++j)
        {
            Ptr<Node> camNode = wifiStaNodes[i].Get(j);
            
            std::string codec_type;
            
            if (codecSelector->GetValue() < 1.0/3.0) {
                codec_type = "H.265";
                
            } else {
                codec_type = "H.264";
                
            }
            
            cameraHelper.SetAttribute("CameraId", UintegerValue(cameraIdCounter));
            cameraHelper.SetAttribute("Codec", StringValue(codec_type));
            ApplicationContainer apps = cameraHelper.Install(camNode);

            Ptr<Ipv4> ipv4 = camNode->GetObject<Ipv4>();
            Ipv4Address ip = ipv4->GetAddress(1, 0).GetLocal();
            serverApp->RegisterClientInfo(ip, YtyServer::ClientInfo(cameraIdCounter, "wifi", regionName, codec_type));

            uint32_t startTimeIdx = WIRED_CAM_PER_REGION + j;
            apps.Start(Seconds(2.0 + (i * totalCamPerRegion + startTimeIdx) * 0.1));
            apps.Stop(Seconds(simulationTime - 2.0));
            cameraIdCounter++;
        }
        
        // --- LTE 摄像头 ---
        for (uint32_t j = 0; j < lteUeNodes[i].GetN(); ++j)
        {
            Ptr<Node> camNode = lteUeNodes[i].Get(j);
            
            
            std::string codec_type;
            
            if (codecSelector->GetValue() < 1.0/3.0) {
                codec_type = "H.265";
            
            } else {
                codec_type = "H.264";
            
            }
            
            cameraHelper.SetAttribute("CameraId", UintegerValue(cameraIdCounter));
            cameraHelper.SetAttribute("Codec", StringValue(codec_type));
            ApplicationContainer apps = cameraHelper.Install(camNode);

            Ptr<Ipv4> ipv4 = camNode->GetObject<Ipv4>();
            Ipv4Address ip = ipv4->GetAddress(1, 0).GetLocal();
            serverApp->RegisterClientInfo(ip, YtyServer::ClientInfo(cameraIdCounter, "lte", regionName, codec_type));
            
            uint32_t startTimeIdx = WIRED_CAM_PER_REGION + WIFI_CAM_PER_REGION + j;
            apps.Start(Seconds(2.0 + (i * totalCamPerRegion + startTimeIdx) * 0.1));
            apps.Stop(Seconds(simulationTime - 2.0));
            cameraIdCounter++;
        }
    }


    // --- 6. 启动仿真 ---
    // AnimationInterface anim("yty_system_animation_beautified.xml");


    // 动态带宽配置与调度
    // 1. 定义一个向量来存储从文件中读取的带宽值
    std::vector<double> bandwidthScheduleKbps;
    // 定义带宽配置文件的名字
    std::ifstream bandwidthFile("scratch/bandwidth.txt"); 

    if (bandwidthFile.is_open())
    {
        double kbps;
        // 循环读取文件中的每一个数字
        while (bandwidthFile >> kbps)
        {
            bandwidthScheduleKbps.push_back(kbps);
        }
        bandwidthFile.close();
        NS_LOG_INFO("成功加载bandwidth.txt");
    }
    else
    {
        NS_LOG_ERROR("打不开bandwidth.txt，使用静态配置");
    }

    // 2. 如果成功读取到带宽数据，则启动动态变化逻辑
    if (!bandwidthScheduleKbps.empty())
    {
        // 定义一个静态索引，用于在调度函数之间保持对当前带宽位置的跟踪
        static uint32_t bandwidthIndex = 0;
        
        // 立即启动第一次带宽变化，后续的调度将由 ScheduleNextBandwidthChange 函数内部循环执行
        // 我们将之前收集的所有汇聚层链路（allCoreToAggDevs）作为修改目标
        Simulator::ScheduleNow(&ScheduleNextBandwidthChange, allCoreToAggDevs, bandwidthScheduleKbps, bandwidthIndex);
    }


    NS_LOG_INFO("仿真开始，持续 " << simulationTime << " 秒...");
    Simulator::Stop(Seconds(simulationTime));
    Simulator::Run();
    Simulator::Destroy();
    NS_LOG_UNCOND("仿真结束。");

    return 0;
}
