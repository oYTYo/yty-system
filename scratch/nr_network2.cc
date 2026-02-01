/*
 * nr_network.cc (PGW-backhaul version)
 *
 * Fix for: "Could not set default value for ns3::Ipv4L3Protocol::IpForward"
 *
 * Key idea:
 *   Do NOT rely on enabling IP forwarding on "remoteHost" (which fails in your build).
 *   Instead, connect the server directly to the EPC PGW node with a P2P backhaul link.
 *   Then all UE <-> Server traffic naturally traverses PGW (router), no extra forwarding needed.
 *
 * Topology:
 *   24 UEs (cameras) --(5G NR)-- 1 gNB -- EPC/PGW --(P2P bottleneck, dynamic bandwidth)-- 1 Server
 *
 * Run:
 *   ./ns3 run "scratch/nr_network -- --time=660 --base=1.5 --traceCameraId=1 --targetCodec=Mixed"
 */

#include "ns3/antenna-module.h"
#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/mobility-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/nr-module.h"

#include "yty-camera-helper.h"
#include "yty-server-helper.h"
#include "yty-camera.h"
#include "yty-server.h"

#include <fstream>
#include <vector>
#include <limits>
#include <string>
#include <cmath>

using namespace ns3;
static constexpr uint32_t CAM_TOTAL = 8;

NS_LOG_COMPONENT_DEFINE("SimpleNrNetwork");

/**
 * @brief 修改一个NetDeviceContainer中所有P2P设备的带宽。
 */
static void
ChangeBandwidth(NetDeviceContainer devices, DataRate newBandwidth)
{
    for (uint32_t i = 0; i < devices.GetN(); ++i)
    {
        Ptr<PointToPointNetDevice> p2pDevice = DynamicCast<PointToPointNetDevice>(devices.Get(i));
        if (p2pDevice)
        {
            p2pDevice->SetDataRate(newBandwidth);
        }
    }
    NS_LOG_UNCOND("At time " << Simulator::Now().GetSeconds()
                             << "s, Bottleneck link bandwidth changed to " << newBandwidth);
}

/**
 * @brief 调度下一次带宽变更事件（每 1 秒一次，循环使用 bandwidth.txt）。
 */
static void
ScheduleNextBandwidthChange(NetDeviceContainer devices,
                            const std::vector<double>& bandwidths_kbps,
                            uint32_t& index,
                            double simulationTime,
                            double baseMultiplier,
                            Ptr<YtyServer> serverApp,
                            bool useOracle)
{
    if (bandwidths_kbps.empty())
    {
        return;
    }
    if (index >= bandwidths_kbps.size())
    {
        NS_LOG_INFO("Bandwidth schedule finished, restarting from the beginning.");
        index = 0;
    }

    double dynamic_multiplier = baseMultiplier;

    // 与 wired/wifi 一致：按 12 摄像头缩放
    double new_kbps = bandwidths_kbps[index] * CAM_TOTAL * 1000 * dynamic_multiplier / 3.5;
    DataRate newRate(std::to_string(new_kbps) + "Kbps");

    ChangeBandwidth(devices, newRate);

    if (useOracle && serverApp)
    {
        serverApp->SetTotalBandwidth(newRate);
    }

    index++;
    Simulator::Schedule(Seconds(1.0),
                        &ScheduleNextBandwidthChange,
                        devices,
                        bandwidths_kbps,
                        index,
                        simulationTime,
                        baseMultiplier,
                        serverApp,
                        useOracle);
}

int
main(int argc, char* argv[])
{
    // 仿真时长/带宽脚本
    double simulationTime = 660.0;
    // double baseMultiplier = 1.5;
    double baseMultiplier = 2.0;
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

    CommandLine cmd(__FILE__);
    cmd.AddValue("useAI", "Enable AI based congestion control", useAI);
    cmd.AddValue("useMinerva", "Enable Minerva", useMinerva);
    cmd.AddValue("useUniQ", "Enable UniQ", useUniQ);
    cmd.AddValue("traceCameraId", "Camera ID to trace at server", traceCameraId);
    cmd.AddValue("algoCameraIdLimit", "Disable bandwidth allocation algorithm for cameras with CameraId > this limit (e.g., 24=all algo, 12=half, 0=all GCC)", algoCameraIdLimit);
    cmd.AddValue("targetCodec", "Force codec (AV1/VP9/H.265/H.264/Mixed)", targetCodec);
    cmd.AddValue("time", "Simulation time (seconds)", simulationTime);
    cmd.AddValue("base", "Base multiplier for dynamic bandwidth", baseMultiplier);
    cmd.AddValue("startLine", "Line number to start reading bandwidth from", startLine);
    cmd.Parse(argc, argv);

    // const uint32_t CAM_TOTAL = 24;
    const uint16_t serverPort = 9;

    // Avoid small RLC buffers becoming a bottleneck
    Config::SetDefault("ns3::NrRlcUm::MaxTxBufferSize", UintegerValue(999999999));

    // --- 1) 节点创建 ---
    NodeContainer serverNode;
    serverNode.Create(1);

    NodeContainer gnbNode;
    gnbNode.Create(1);

    NodeContainer ueNodes;
    ueNodes.Create(CAM_TOTAL);

    // --- 2) Mobility（与 wifi 版相似：UE 在 5~50m 圆盘内） ---
    MobilityHelper mobility;

    Ptr<ListPositionAllocator> fixedAlloc = CreateObject<ListPositionAllocator>();
    fixedAlloc->Add(Vector(0.0, 0.0, 0.0));   // Server
    fixedAlloc->Add(Vector(0.0, 0.0, 10.0));  // gNB
    mobility.SetPositionAllocator(fixedAlloc);
    mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
    mobility.Install(serverNode);
    mobility.Install(gnbNode);

    Ptr<RandomDiscPositionAllocator> discAlloc = CreateObject<RandomDiscPositionAllocator>();
    discAlloc->SetX(0.0);
    discAlloc->SetY(0.0);
    discAlloc->SetRho(CreateObjectWithAttributes<UniformRandomVariable>(
        "Min", DoubleValue(5.0), "Max", DoubleValue(50.0)));
    mobility.SetPositionAllocator(discAlloc);
    mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
    mobility.Install(ueNodes);

    // --- 3) NR + EPC 配置 ---
    Ptr<NrPointToPointEpcHelper> epcHelper = CreateObject<NrPointToPointEpcHelper>();
    Ptr<IdealBeamformingHelper> bfHelper = CreateObject<IdealBeamformingHelper>();
    Ptr<NrHelper> nrHelper = CreateObject<NrHelper>();

    nrHelper->SetEpcHelper(epcHelper);
    nrHelper->SetBeamformingHelper(bfHelper);

    bfHelper->SetAttribute("BeamformingMethod",
                           TypeIdValue(DirectPathBeamforming::GetTypeId()));
    epcHelper->SetAttribute("S1uLinkDelay", TimeValue(MilliSeconds(0)));

    // 天线（与 cttc-nr-demo 一致）
    nrHelper->SetUeAntennaAttribute("NumRows", UintegerValue(2));
    nrHelper->SetUeAntennaAttribute("NumColumns", UintegerValue(4));
    nrHelper->SetUeAntennaAttribute("AntennaElement",
                                    PointerValue(CreateObject<IsotropicAntennaModel>()));

    nrHelper->SetGnbAntennaAttribute("NumRows", UintegerValue(4));
    nrHelper->SetGnbAntennaAttribute("NumColumns", UintegerValue(8));
    nrHelper->SetGnbAntennaAttribute("AntennaElement",
                                     PointerValue(CreateObject<IsotropicAntennaModel>()));

    // 单 band / 单 CC / 单 BWP
    uint16_t numerology = 2;
    double centralFrequency = 28e9;
    double bandwidth = 100e6;
    double totalTxPowerDbm = 35;

    CcBwpCreator ccBwpCreator;
    const uint8_t numCcPerBand = 1;
    CcBwpCreator::SimpleOperationBandConf bandConf(centralFrequency, bandwidth, numCcPerBand);
    OperationBandInfo band = ccBwpCreator.CreateOperationBandContiguousCc(bandConf);

    Ptr<NrChannelHelper> channelHelper = CreateObject<NrChannelHelper>();
    channelHelper->ConfigureFactories("UMi", "Default", "ThreeGpp");
    channelHelper->SetChannelConditionModelAttribute("UpdatePeriod", TimeValue(MilliSeconds(0)));
    channelHelper->SetPathlossAttribute("ShadowingEnabled", BooleanValue(false));
    channelHelper->AssignChannelsToBands({band});

    BandwidthPartInfoPtrVector allBwps = CcBwpCreator::GetAllBwps({band});

    // bearer -> BWP0
    nrHelper->SetGnbBwpManagerAlgorithmAttribute("NGBR_LOW_LAT_EMBB", UintegerValue(0));
    nrHelper->SetUeBwpManagerAlgorithmAttribute("NGBR_LOW_LAT_EMBB", UintegerValue(0));

    // 安装 NR 设备
    NetDeviceContainer gnbDevs = nrHelper->InstallGnbDevice(gnbNode, allBwps);
    NetDeviceContainer ueDevs = nrHelper->InstallUeDevice(ueNodes, allBwps);

    // gNB PHY: numerology + TxPower
    double x = std::pow(10.0, totalTxPowerDbm / 10.0);
    nrHelper->GetGnbPhy(gnbDevs.Get(0), 0)->SetAttribute("Numerology", UintegerValue(numerology));
    nrHelper->GetGnbPhy(gnbDevs.Get(0), 0)->SetAttribute("TxPower", DoubleValue(10 * std::log10(x)));

    // --- 4) IP 协议栈 ---
    InternetStackHelper internet;
    internet.Install(serverNode);
    internet.Install(ueNodes);

    // UE 分配 IP（EPC 默认 7.0.0.0/8）
    Ipv4InterfaceContainer ueIfaces = epcHelper->AssignUeIpv4Address(NetDeviceContainer(ueDevs));

    // UE attach
    nrHelper->AttachToClosestGnb(ueDevs, gnbDevs);

    // UE 默认路由（指向 EPC 网关）
    Ipv4StaticRoutingHelper ipv4RoutingHelper;
    for (uint32_t i = 0; i < ueNodes.GetN(); ++i)
    {
        Ptr<Ipv4StaticRouting> ueRouting =
            ipv4RoutingHelper.GetStaticRouting(ueNodes.Get(i)->GetObject<Ipv4>());
        ueRouting->SetDefaultRoute(epcHelper->GetUeDefaultGatewayAddress(), 1);
    }

    // --- 5) “回传瓶颈链路”：PGW <-> Server ---
    Ptr<Node> pgw = epcHelper->GetPgwNode();
    NS_ASSERT(pgw != nullptr);

    PointToPointHelper p2pBackhaul;
    p2pBackhaul.SetDeviceAttribute("DataRate", StringValue("100Mbps"));
    p2pBackhaul.SetChannelAttribute("Delay", StringValue("5ms"));
    p2pBackhaul.SetQueue("ns3::DropTailQueue", "MaxSize", StringValue("150p"));

    NetDeviceContainer backhaulDevs = p2pBackhaul.Install(pgw, serverNode.Get(0));

    // 分配 IP（与 wired/wifi 对齐：10.1.1.0/30）
    Ipv4AddressHelper ipv4h;
    ipv4h.SetBase("10.1.1.0", "255.255.255.252");
    Ipv4InterfaceContainer pgwToServerIfaces = ipv4h.Assign(backhaulDevs);
    Ipv4Address pgwIp = pgwToServerIfaces.GetAddress(0);
    Ipv4Address serverIp = pgwToServerIfaces.GetAddress(1);

    // Server 默认路由指向 PGW（与 wifi/wired 逻辑一致）
    Ptr<Ipv4StaticRouting> serverRouting =
        ipv4RoutingHelper.GetStaticRouting(serverNode.Get(0)->GetObject<Ipv4>());
    serverRouting->SetDefaultRoute(pgwIp, 1);

    // Ipv4GlobalRoutingHelper::PopulateRoutingTables();

    // --- 6) 安装应用程序 ---
    LogComponentEnable("YtyServerApplication", LOG_LEVEL_INFO);

    YtyServerHelper serverHelper(serverPort);
    serverHelper.SetAttribute("LogFile", StringValue("scratch/play_status_nr2.txt"));
    serverHelper.SetAttribute("UseAI", BooleanValue(useAI));
    serverHelper.SetAttribute("UseMinerva", BooleanValue(useMinerva));
    serverHelper.SetAttribute("UseOracle", BooleanValue(useOracle));
    serverHelper.SetAttribute("UseUniQ", BooleanValue(useUniQ));
    serverHelper.SetAttribute("TraceCameraId", UintegerValue(traceCameraId));

    serverHelper.SetAttribute("AlgoCameraIdLimit", UintegerValue(algoCameraIdLimit));
    ApplicationContainer serverApps = serverHelper.Install(serverNode.Get(0));
    serverApps.Start(Seconds(1.0));
    serverApps.Stop(Seconds(simulationTime - 1.0));

    Ptr<YtyServer> serverApp = DynamicCast<YtyServer>(serverApps.Get(0));
    NS_ASSERT(serverApp != nullptr);

    YtyCameraHelper cameraHelper(serverIp, serverPort);
    uint32_t cameraIdCounter = 1;

    for (uint32_t i = 0; i < ueNodes.GetN(); ++i)
    {
        Ptr<Node> camNode = ueNodes.Get(i);

        std::string codec_type;
        if (targetCodec != "Mixed")
        {
            codec_type = targetCodec;
        }
        else
        {
            if (i % 4 == 0) codec_type = "H.264";
            else if (i % 4 == 1) codec_type = "H.265";
            else if (i % 4 == 2) codec_type = "VP9";
            else codec_type = "AV1";
        }

        cameraHelper.SetAttribute("CameraId", UintegerValue(cameraIdCounter));
        cameraHelper.SetAttribute("Codec", StringValue(codec_type));
        ApplicationContainer apps = cameraHelper.Install(camNode);

        Ipv4Address ip = ueIfaces.GetAddress(i);
        serverApp->RegisterClientInfo(ip,
            YtyServer::ClientInfo(cameraIdCounter, "nr", "NR-Region", codec_type));

        apps.Start(Seconds(2.0 + i * 0.2));
        apps.Stop(Seconds(simulationTime - 2.0));
        cameraIdCounter++;
    }

    // --- 7) 动态带宽读取与调度（作用在 PGW<->Server 瓶颈链路上） ---
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
        NS_LOG_INFO("Loaded bandwidth schedule from scratch/bandwidth.txt");
    }
    else
    {
        NS_LOG_ERROR("Could not open scratch/bandwidth.txt. Using static bandwidth.");
    }

    if (!bandwidthScheduleKbps.empty())
    {
        static uint32_t bandwidthIndex = (startLine > 0) ? (startLine - 1) : 0;
        Simulator::ScheduleNow(&ScheduleNextBandwidthChange, backhaulDevs, bandwidthScheduleKbps, bandwidthIndex, simulationTime, baseMultiplier, serverApp, useOracle);
    }

    // --- 8) 启动仿真 ---
    Simulator::Stop(Seconds(simulationTime));
    Simulator::Run();
    Simulator::Destroy();
    NS_LOG_UNCOND("Simulation finished.");
    return 0;
}
