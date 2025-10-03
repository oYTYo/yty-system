/*
 * simple-network.cc
 *
 * 网络描述:
 * 这是一个简化的摄像头监控网络仿真脚本，参考了原有的 large-scale-test.cc。
 * 拓扑结构被简化为二级星型拓扑: "摄像头 - 交换机 - 服务器"。
 *
 * 主要特点:
 * 1. 拓扑: 9个有线摄像头 -> 1个中心交换机 -> 1个服务器。
 * 2. 摄像头配置: 包含6个H.264编码的摄像头和3个H.265编码的摄像头。
 * 3. 动态带宽: 交换机到服务器之间的骨干链路带宽会根据 'scratch/bandwidth.txt' 文件中的配置动态变化，模拟网络波动。
 * 4. 功能保留: 保留了原有的 YtyCamera 和 YtyServer 应用逻辑，以及通过命令行开启 AI 或 Minerva 拥塞控制算法的选项。
 *
 */

#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/applications-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/csma-module.h" // 尽管我们主要用P2P，但保留以防未来扩展
#include "ns3/mobility-module.h"

// 包含自定义的应用助手和应用头文件
#include "yty-camera-helper.h"
#include "yty-server-helper.h"
#include "yty-camera.h"
#include "yty-server.h"

#include <fstream>
#include <vector>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("SimpleWiredCameraNetwork");

// ===================================================================================
// --- [功能保留] 动态带宽调整函数 ---
// 这部分函数与原版 network.cc 完全相同，用于实现链路带宽的动态调整。
// ===================================================================================

/**
 * @brief 修改一个NetDeviceContainer中所有P2P设备的带宽。
 * @param devices 要修改的链路设备容器。
 * @param newBandwidth 新的带宽值。
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
    NS_LOG_UNCOND("At time " << Simulator::Now().GetSeconds() << "s, Bottleneck link bandwidth changed to " << newBandwidth);
}

/**
 * @brief 调度下一次带宽变更事件。
 *
 * 从带宽值向量中读取下一个值，设置链路带宽，并安排1秒后再次调用自身，形成循环。
 * @param devices 目标链路设备。
 * @param bandwidths_kbps 从文件中读取的带宽值向量。
 * @param index 当前在向量中的位置（使用引用传递，以便修改）。
 */
void ScheduleNextBandwidthChange(NetDeviceContainer devices, const std::vector<double>& bandwidths_kbps, uint32_t& index)
{
    if (index >= bandwidths_kbps.size()) {
        NS_LOG_INFO("Bandwidth schedule finished, restarting from the beginning.");
        index = 0;
    }

    // [修改] 带宽计算因子调整，原先是为60个摄像头设计的，现在调整为9个
    double new_kbps = bandwidths_kbps[index] * 9 * 1000 * 10.0 / 3.5;
    DataRate newRate(std::to_string(new_kbps) + "Kbps");

    ChangeBandwidth(devices, newRate);
    index++;

    Simulator::Schedule(Seconds(1.0), &ScheduleNextBandwidthChange, devices, bandwidths_kbps, index);
}


int main(int argc, char* argv[])
{
    // --- [功能保留] AI 和 Minerva 模式开关 ---
    bool useAI = false;
    bool useMinerva = false;
    CommandLine cmd;
    cmd.AddValue("useAI", "Enable AI-based congestion control", useAI);
    cmd.AddValue("useMinerva", "Enable Minerva-like QoE-based rate adjustment", useMinerva);
    cmd.Parse(argc, argv);

    // --- 仿真核心参数 ---
    const uint32_t WIRED_CAM_TOTAL = 9; // [修改] 简化为9个有线摄像头
    const double   simulationTime  = 660.0;
    const uint16_t serverPort      = 9;

    // --- 1. 节点创建 ---
    NS_LOG_INFO("Creating simplified network nodes...");
    NodeContainer serverNode, switchNode;
    serverNode.Create(1);
    switchNode.Create(1);

    NodeContainer cameraNodes;
    cameraNodes.Create(WIRED_CAM_TOTAL);

    // --- 2. 安装协议栈 ---
    NS_LOG_INFO("Installing IP protocol stack...");
    InternetStackHelper internet;
    internet.Install(serverNode);
    internet.Install(switchNode);
    internet.Install(cameraNodes);

    // --- 3. 配置网络链路和IP地址 ---
    NS_LOG_INFO("Configuring network links and devices...");

    PointToPointHelper p2pCamToSwitch, p2pSwitchToServer;

    // (3.1) 配置摄像头到交换机的链路 (高带宽，低延迟)
    p2pCamToSwitch.SetDeviceAttribute("DataRate", StringValue("100Mbps"));
    p2pCamToSwitch.SetChannelAttribute("Delay", StringValue("1ms"));
    p2pCamToSwitch.SetQueue("ns3::DropTailQueue", "MaxSize", StringValue("50p"));

    // (3.2) 配置交换机到服务器的链路 (这将是我们的瓶颈链路)
    // 初始带宽可以设为一个基准值，后续会由动态调整函数覆盖
    p2pSwitchToServer.SetDeviceAttribute("DataRate", StringValue("50Mbps"));
    p2pSwitchToServer.SetChannelAttribute("Delay", StringValue("5ms"));
    p2pSwitchToServer.SetQueue("ns3::DropTailQueue", "MaxSize", StringValue("150p"));

    Ipv4AddressHelper ipv4h;
    Ipv4StaticRoutingHelper ipv4RoutingHelper;

    // (3.3) 连接交换机到服务器
    NetDeviceContainer switchToServerDevs = p2pSwitchToServer.Install(switchNode.Get(0), serverNode.Get(0));
    ipv4h.SetBase("10.1.1.0", "255.255.255.252");
    Ipv4InterfaceContainer switchToServerIfaces = ipv4h.Assign(switchToServerDevs);
    Ipv4Address serverIp = switchToServerIfaces.GetAddress(1);
    Ipv4Address switchIp_onServerLink = switchToServerIfaces.GetAddress(0);

    // 在服务器上设置默认路由，指向交换机
    ipv4RoutingHelper.GetStaticRouting(serverNode.Get(0)->GetObject<Ipv4>())->SetDefaultRoute(switchIp_onServerLink, 1);
    
    // (3.4) 连接所有摄像头到交换机
    Ptr<Ipv4StaticRouting> switchRouting = ipv4RoutingHelper.GetStaticRouting(switchNode.Get(0)->GetObject<Ipv4>());
    // 交换机的默认路由指向服务器
    switchRouting->SetDefaultRoute(serverIp, 1);

    for (uint32_t i = 0; i < WIRED_CAM_TOTAL; ++i)
    {
        NetDeviceContainer camToSwitchDevs = p2pCamToSwitch.Install(cameraNodes.Get(i), switchNode.Get(0));
        
        // 为每个摄像头到交换机的链路分配一个独立的 /30 子网
        std::string ipBase = "192.168." + std::to_string(i + 1) + ".0";
        ipv4h.SetBase(ipBase.c_str(), "255.255.255.252");
        Ipv4InterfaceContainer camToSwitchIfaces = ipv4h.Assign(camToSwitchDevs);

        Ipv4Address camIp = camToSwitchIfaces.GetAddress(0);
        Ipv4Address switchIp_onCamLink = camToSwitchIfaces.GetAddress(1);

        // 在摄像头上设置默认路由，指向交换机
        ipv4RoutingHelper.GetStaticRouting(cameraNodes.Get(i)->GetObject<Ipv4>())->SetDefaultRoute(switchIp_onCamLink, 1);

        // 在交换机上添加指向该摄像头子网的路由
        switchRouting->AddNetworkRouteTo(Ipv4Address(ipBase.c_str()), Ipv4Mask("255.255.255.252"), i + 2); // 接口索引从2开始
    }

    // --- 4. 安装应用程序 ---
    NS_LOG_INFO("Installing camera and server applications...");

    // (4.1) 服务器应用
    LogComponentEnable("YtyServerApplication", LOG_LEVEL_INFO);
    YtyServerHelper serverHelper(serverPort);
    serverHelper.SetAttribute("LogFile", StringValue("scratch/play_status_simple.txt"));
    serverHelper.SetAttribute("UseAI", BooleanValue(useAI));
    serverHelper.SetAttribute("UseMinerva", BooleanValue(useMinerva));

    ApplicationContainer serverApps = serverHelper.Install(serverNode.Get(0));
    serverApps.Start(Seconds(1.0));
    serverApps.Stop(Seconds(simulationTime - 1.0));

    Ptr<YtyServer> serverApp = serverApps.Get(0)->GetObject<YtyServer>();
    NS_ASSERT(serverApp != nullptr);

    // (4.2) 摄像头应用
    YtyCameraHelper cameraHelper(serverIp, serverPort);
    uint32_t cameraIdCounter = 1;

    for (uint32_t i = 0; i < WIRED_CAM_TOTAL; ++i)
    {
        Ptr<Node> camNode = cameraNodes.Get(i);
        
        // --- [核心修改] 固定分配6个H.264和3个H.265摄像头 ---
        std::string codec_type;
        if (i < 6) {
            codec_type = "H.264";
        } else {
            codec_type = "H.265";
        }
        
        cameraHelper.SetAttribute("CameraId", UintegerValue(cameraIdCounter));
        cameraHelper.SetAttribute("Codec", StringValue(codec_type));
        ApplicationContainer apps = cameraHelper.Install(camNode);
        
        Ptr<Ipv4> ipv4 = camNode->GetObject<Ipv4>();
        Ipv4Address ip = ipv4->GetAddress(1, 0).GetLocal();
        // 注册客户端信息到服务器
        serverApp->RegisterClientInfo(ip, YtyServer::ClientInfo(cameraIdCounter, "wired", "Simplified-Region", codec_type));

        // 错开启动时间
        apps.Start(Seconds(2.0 + i * 0.2));
        apps.Stop(Seconds(simulationTime - 2.0));
        cameraIdCounter++;
    }

    // --- 5. [修改] 配置与调度动态带宽 ---
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
        NS_LOG_ERROR("Could not open scratch/bandwidth.txt. Using static bandwidth.");
    }

    // 如果成功读取到带宽数据，则启动动态变化逻辑
    if (!bandwidthScheduleKbps.empty())
    {
        static uint32_t bandwidthIndex = 0;
        // [修改] 将动态带宽应用在交换机到服务器的链路上
        Simulator::ScheduleNow(&ScheduleNextBandwidthChange, switchToServerDevs, bandwidthScheduleKbps, bandwidthIndex);
    }

    // --- 6. 启动仿真 ---
    NS_LOG_INFO("Starting simulation for " << simulationTime << " seconds...");
    Simulator::Stop(Seconds(simulationTime));
    Simulator::Run();
    Simulator::Destroy();
    NS_LOG_UNCOND("Simulation finished.");

    return 0;
}