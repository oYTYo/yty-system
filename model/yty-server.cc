/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */

#include "yty-server.h"
#include "ns3/log.h"
#include "ns3/boolean.h"
#include "ns3/ipv4-address.h"
#include "ns3/nstime.h"
#include "ns3/inet-socket-address.h"
#include "ns3/socket.h"
#include "ns3/simulator.h"
#include "ns3/socket-factory.h"
#include "ns3/packet.h"
#include "ns3/uinteger.h"
#include "ns3/double.h"

#include "ns3/string.h"
#include <fstream>
#include <string>
#include <cmath> // 包含 cmath 以使用 std::abs

#include "yty-camera.h" // 包含摄像头头文件以使用 RtpHeader

#include <algorithm>
#include <iomanip>

#include <cmath> // 确保包含了 cmath 以使用 exp 和 log
#include "ns3/data-rate.h"

#include <nlohmann/json.hpp>
// 为方便使用，创建一个别名
using json = nlohmann::json;

namespace ns3 {

namespace {
// Minerva的平均带宽效用函数
// (y_monotonic_avg):
static const std::vector<double> g_minerva_qoe_data = {
    72.68324625555556, 80.33331619791667, 83.11193343289689, 86.69318606493506, 
    89.1289354265403, 90.93413108024691, 91.75154233286908, 91.8827452559055, 
    92.54758171052632, 92.54758171052632, 93.34413635135135, 93.34413635135135, 
    93.92623229761905, 94.11436329861111, 94.20016699275362, 95.01230799107142, 
    95.01230799107142, 95.67521721428571, 95.67521721428571, 95.67521721428571, 
    95.67521721428571, 95.67521721428571, 95.67521721428571, 95.67521721428571, 
    95.67521721428571
};

// Bitrate Data in Mbps (x_binned_avg):
static const std::vector<double> g_minerva_bitrate_data_mbps = {
    1.122184577777778, 1.3805583974358975, 1.5900500327332243, 1.8224328571428572, 
    2.069220213270142, 2.3209053827160493, 2.5467087465181057, 2.763488162729659, 
    2.9998911943319837, 3.2466350127226464, 3.492118108108108, 3.7161873188405794, 
    3.956822333333333, 4.17658125, 4.413889927536232, 4.659185, 
    4.8799874999999995, 5.091888, 5.375009199999999, 5.597381666666667, 
    5.824596666666667, 6.059490769230769, 6.3447216666666675, 6.562622, 
    6.845560909090909
};


/**
 * @brief 线性插值函数，等同于 numpy.interp(target_y, y_vec, x_vec)
 * @param target_y 目标 QoE (y 轴)
 * @param y_vec 已知的 QoE 向量 (y 轴, 必须单调递增)
 * @param x_vec 已知的 Bitrate 向量 (x 轴)
 * @return double 插值后的 Bitrate (单位: Mbps)
 */
static double interpolate_qoe_to_bitrate_mbps(double target_y, const std::vector<double>& y_vec, const std::vector<double>& x_vec)
{
    if (y_vec.empty() || x_vec.empty() || y_vec.size() != x_vec.size()) {
        // 异常处理：返回一个安全的回退值 (例如 1.0 Mbps)
        return 1.0; 
    }

    // 1. 处理边界情况 (QoE 超出拟合范围)
    if (target_y <= y_vec.front()) {
        return x_vec.front();
    }
    if (target_y >= y_vec.back()) {
        return x_vec.back();
    }

    // 2. 查找插值区间
    // 找到第一个 *不小于* target_y 的元素
    auto it = std::lower_bound(y_vec.begin(), y_vec.end(), target_y);

    // 如果精确匹配
    if (*it == target_y) {
        size_t i = std::distance(y_vec.begin(), it);
        return x_vec[i];
    }

    // 3. 执行线性插值
    // target_y 位于 (it-1) 和 (it) 之间
    size_t i2 = std::distance(y_vec.begin(), it);
    size_t i1 = i2 - 1;

    double y2 = y_vec[i2];
    double y1 = y_vec[i1];
    double x2 = x_vec[i2];
    double x1 = x_vec[i1];

    // 防止除以零 (尽管 y_vec 单调递增，理论上不会发生)
    if (y2 == y1) {
        return x1;
    }

    // 插值比例
    double ratio = (target_y - y1) / (y2 - y1);
    
    // 计算对应的 x (Bitrate in Mbps)
    return x1 + ratio * (x2 - x1);
}


// +++ 新增: 状态聚合辅助函数 +++
std::string AggregateState(const std::vector<TransportMetricSample>& samples, 
                           double avgVmaf, 
                           double stutterRate, 
                           double vmafJitter, // <-- 新增 vmafJitter
                           const std::string& codec) 
{
    json state_json;

    // 1. (新) 序列化完整的50ms指标样本
    json metric_samples_json = json::array();
    for (const auto& s : samples) {
        metric_samples_json.push_back({
            s.throughputKbps,
            s.delayMs,
            s.lossRate,
            s.jitterMs
        });
    }
    state_json["metric_samples"] = metric_samples_json; // 这是一个 20x4 的数组

    // 2. (新) 序列化宏观标量
    state_json["last_vmaf"] = avgVmaf;
    state_json["last_stutter_rate"] = stutterRate;
    state_json["last_vmaf_jitter"] = vmafJitter; // <-- 新增
    state_json["codec"] = codec;
    
    return state_json.dump();
}


} // 匿名命名空间结束
// Minerva 集成代码结束


NS_LOG_COMPONENT_DEFINE("YtyServerApplication");
NS_OBJECT_ENSURE_REGISTERED(YtyServer);


// 码率的绝对上限和下限，防止码率无限增长或低到无意义
const double MAX_BITRATE_MBPS = 25.0; // 码率最高不超过 10 Mbps
const double MIN_BITRATE_KBPS = 300.0; // 码率最低不低于 100 Kbps  // 2025.11.26: 如果陷入保底码率难以恢复，修改这个值或许有用。

// 单位换算常量
const double BPS_IN_KBPS = 1000.0; // 1 Kbps = 1000 bps

// 基于丢包控制的两个核心阈值
const double LOSS_RATE_THRESHOLD_LOW = 0.001;   // 丢包率低于 0.1% 时，网络被认为是“健康的”，允许码率增加
const double LOSS_RATE_THRESHOLD_HIGH = 0.01;   // 丢包率高于 1% 时，网络被认为是“严重拥塞的”，必须降低码率

// 基于延迟控制的核心初始阈值 (gamma)
const double OVERUSE_THRESHOLD_MS_INITIAL = 12.5; // 延迟变化趋势的初始判断阈值 (单位: 毫秒)

// 定义一个标准的数据包大小（字节），用于加性增的计算。
const double AVERAGE_PACKET_SIZE_BYTES = 1200.0;


// --- GCCController 构造函数实现 ---
// (这部分也要从 gcc_server.cpp 迁移过来)
GCCController::GCCController(double start_bitrate_kbps)
    : current_bitrate_bps_(start_bitrate_kbps * BPS_IN_KBPS),
      last_acked_bitrate_bps_(0.0),
      last_update_ms_(-1),
      last_group_arrival_time_ms_(-1),
      last_group_timestamp_ms_(-1),
      overuse_threshold_ms_(OVERUSE_THRESHOLD_MS_INITIAL),
      state_(NetworkState::Normal),
      time_of_last_bitrate_increase_ms_(-1) {}


// --- GCCController 方法实现 (精确复制自 gcc_server.cpp 并适配时间源) ---

std::string GCCController::loss_based_control(double loss_rate) {
    if (loss_rate > LOSS_RATE_THRESHOLD_HIGH) return "decrease";
    if (loss_rate < LOSS_RATE_THRESHOLD_LOW) return "increase";
    return "hold";
}

std::string GCCController::delay_based_control(double delay_ms, long long current_time_ms) {
    if (last_group_arrival_time_ms_ == -1) {
        last_group_arrival_time_ms_ = current_time_ms;
        last_group_timestamp_ms_ = current_time_ms - static_cast<long long>(delay_ms);
        return "hold";
    }

    long long arrival_delta_ms = current_time_ms - last_group_arrival_time_ms_;
    long long estimated_send_time_ms = current_time_ms - static_cast<long long>(delay_ms);
    long long timestamp_delta_ms = estimated_send_time_ms - last_group_timestamp_ms_;
    
    double delay_variation_ms = static_cast<double>(arrival_delta_ms - timestamp_delta_ms);
    
    last_group_arrival_time_ms_ = current_time_ms;
    last_group_timestamp_ms_ = estimated_send_time_ms;

    if (delay_variation_ms > overuse_threshold_ms_) {
        state_ = NetworkState::Overuse;
    } else if (std::abs(delay_variation_ms) < overuse_threshold_ms_) {
        state_ = NetworkState::Normal;
    } else { // delay_variation_ms < -overuse_threshold_ms_
        state_ = NetworkState::Underuse;
    }
    
    double K_u = (overuse_threshold_ms_ < 6) ? 0.01 : 0.00018;
    double K_d = 0.039;
    double update_rate = (std::abs(delay_variation_ms) > overuse_threshold_ms_) ? K_u : K_d;
    overuse_threshold_ms_ += (std::abs(delay_variation_ms) - overuse_threshold_ms_) * update_rate;

    // 这里操作时延敏感度，默认是6
    overuse_threshold_ms_ = std::max(6.0, std::min(overuse_threshold_ms_, 600.0));

    if (state_ == NetworkState::Overuse) return "decrease";
    if (state_ == NetworkState::Normal) return "increase";
    if (state_ == NetworkState::Underuse) return "increase";
    
    return "hold";
}

// get_target_bitrate_kbps 接口适配 ns-3 参数
GCCResult GCCController::get_target_bitrate_kbps(double throughputKbps, double delayMs, double lossRate, double rttMs, long long currentTimeMs, double minervaWeight) {
    // 移除原始 gcc_server.cpp 中的 JSON 解析部分，直接使用传入的参数
    last_acked_bitrate_bps_ = throughputKbps * BPS_IN_KBPS;
    
    std::string loss_decision = loss_based_control(lossRate);
    std::string delay_decision = delay_based_control(delayMs, currentTimeMs);
    
    update_bitrate(loss_decision, delay_decision, rttMs, currentTimeMs, minervaWeight);
    
    current_bitrate_bps_ = std::max(current_bitrate_bps_, MIN_BITRATE_KBPS * BPS_IN_KBPS);
    current_bitrate_bps_ = std::min(current_bitrate_bps_, MAX_BITRATE_MBPS * 1e6); // 1e6 是 MBPS 到 BPS

    // 创建并填充GCCResult结构体
    GCCResult result;
    result.target_bitrate_kbps = current_bitrate_bps_ / BPS_IN_KBPS;
    result.loss_decision = loss_decision;
    result.delay_decision = delay_decision;

    // 返回这个包含所有结果的结构体
    return result;
}


// // 实现强制重置GCC吞吐量逻辑
// void GCCController::ResetState(double bitrate_kbps) {
//     // 强制将当前估算码率设置为传入的值（探测码率）
//     current_bitrate_bps_ = bitrate_kbps * BPS_IN_KBPS;
    
//     // 关键：同时更新 last_acked，让算法认为当前吞吐量就是这么低
//     last_acked_bitrate_bps_ = current_bitrate_bps_;
    
//     // 重置状态为 Normal，清除过载标记
//     state_ = NetworkState::Normal;
    
//     // 重置最后增加时间，允许算法立即开始重新评估
//     time_of_last_bitrate_increase_ms_ = -1;
    
//     // 重置过载阈值，给它一个较宽松的开始
//     overuse_threshold_ms_ = OVERUSE_THRESHOLD_MS_INITIAL;
// }


std::string GCCController::get_state_string() const {
    switch (state_) {
        case NetworkState::Normal: return "Normal";
        case NetworkState::Overuse: return "Overuse";
        case NetworkState::Underuse: return "Underuse";
        default: return "Unknown";
    }
}

void GCCController::update_bitrate(const std::string& loss_decision, const std::string& delay_decision, double rtt_ms, long long current_time_ms, double minervaWeight) {

    // 为了防止权重过大或过小导致算法不稳定，我们将其限制在一个合理的范围内，例如 [0.5, 2.0]，与Minerva论文一致
    double clampedWeight = std::max(0.5, std::min(minervaWeight, 2.0));

    if (loss_decision == "decrease" || delay_decision == "decrease") {

        // 这个0.85是默认是乘性减幅度
        double decreaseFactor = 1.0 - ((1.0 - 0.85) / clampedWeight);  // 一个动态衰减因子，当w等于1的时候不改变衰减幅度，w大于1衰减变少，w小于1衰减变多。Minerva只修改了乘性减的幅度

        current_bitrate_bps_ = std::min(
            current_bitrate_bps_,
            std::max(last_acked_bitrate_bps_ * decreaseFactor, MIN_BITRATE_KBPS * BPS_IN_KBPS)
        );
        time_of_last_bitrate_increase_ms_ = -1;
        return;
    }

    if (loss_decision == "increase" && delay_decision == "increase") {
        if (state_ == NetworkState::Normal) {
            double time_delta_seconds = (last_update_ms_ > 0) ? 
                                        (current_time_ms - last_update_ms_) / 1000.0 : 
                                        0.02; 

            double response_time_ms = 100.0 + rtt_ms;
            // 还原为 gcc_server.cpp 中的值：0.5，这个alpha是加性增的幅度
            double alpha = 0.5 * time_delta_seconds; 
            // 还原为 gcc_server.cpp 中的值：50000.0
            double additive_increase_bps = std::max(50000.0, alpha * (AVERAGE_PACKET_SIZE_BYTES * 8000.0) / response_time_ms);
            
            // current_bitrate_bps_ += additive_increase_bps;
            current_bitrate_bps_ += (additive_increase_bps * clampedWeight);  // 可选项：修改加性增的幅度

        } else { // state_ == NetworkState::Underuse
            // 还原为 gcc_server.cpp 中的值：1.15
            current_bitrate_bps_ *= 1.15;
            // double increaseFactor = 1.0 + ((1.15 - 1.0) * clampedWeight);  // 可选项：修改乘性增的速度
            // current_bitrate_bps_ *= increaseFactor;
        }
    }
    last_update_ms_ = current_time_ms;
}


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
                      MakeTimeChecker())
        // 为日志记录添加新属性
        .AddAttribute("LogFile", "File to log playback statistics.",
                      StringValue("scratch/play_status.txt"),
                      MakeStringAccessor(&YtyServer::m_logFileName),
                      MakeStringChecker())
        .AddAttribute("LogInterval", "Interval for logging playback stats.",
                      TimeValue(Seconds(1.0)),
                      MakeTimeAccessor(&YtyServer::m_logInterval),
                      MakeTimeChecker())
        .AddAttribute("UseAI", "Enable AI-based congestion control via ZMQ.",
                      BooleanValue(false), // 默认关闭AI模式
                      MakeBooleanAccessor(&YtyServer::m_useAI),
                      MakeBooleanChecker())
        .AddAttribute("UseOracle", "Enable Oracle (all-knowing) congestion control.",
                      BooleanValue(false), // 默认关闭
                      MakeBooleanAccessor(&YtyServer::m_useOracle),
                      MakeBooleanChecker())
        .AddAttribute("UseMinerva", "Enable Minerva-like QoE-based rate adjustment.",
                      BooleanValue(false), // 默认关闭 Minerva
                      MakeBooleanAccessor(&YtyServer::m_useMinerva),
                      MakeBooleanChecker())
        .AddAttribute("UseUniQ", "Enable UniQ algorithm via ZMQ.",
                      BooleanValue(false),
                      MakeBooleanAccessor(&YtyServer::m_useUniQ),
                      MakeBooleanChecker())
        .AddAttribute("AlgoCameraIdLimit", 
                      "The max Camera ID that allows AI/Minerva/UniQ algorithms. Cameras with ID > this value will use default GCC (weight=1.0). Default is infinity.",
                      UintegerValue(UINT32_MAX), // 默认为最大整数，保证不传参时所有摄像头都生效
                      MakeUintegerAccessor(&YtyServer::m_algoCameraIdLimit),
                      MakeUintegerChecker<uint32_t>())
        .AddAttribute("TraceCameraId", "Camera ID to trace for congestion control debugging. (0 = disabled)",
                      UintegerValue(0),
                      MakeUintegerAccessor(&YtyServer::m_traceCameraId),
                      MakeUintegerChecker<uint32_t>());
    return tid;
}


YtyServer::YtyServer() : m_useOracle(false), m_useMinerva(false), m_useAI(false), m_socket(0), m_port(9), m_totalOracleBandwidth(DataRate("0bps")), m_totalCodecWeight(0.0)
{

    m_algoCameraIdLimit = UINT32_MAX;

    // 如果启用了AI模式，则初始化ZMQ上下文
    if (m_useAI) {
        m_zmq_context = std::make_unique<zmq::context_t>(1);
    }
    // 在构造函数中初始化VMAF查询表
    InitializeVmafLut();
}

YtyServer::~YtyServer() { 
    // 清理 ZMQ sockets
    m_zmq_sockets.clear();
    m_socket = 0; 
}


void YtyServer::DoDispose(void)
{
    Application::DoDispose();
}

// VMAF 查询表初始化函数的具体实现
void YtyServer::InitializeVmafLut()
{
    // 这个函数的内容就是将 fit_QoE.py 中的 VMAF_LUT 硬编码到 C++ 代码中
    // H.264
    m_vmafLut["H.264"][{854, 480}] = {{18, 72.36}, {19, 71.39}, {20, 70.38}, {21, 69.25}, {22, 67.99}, {23, 66.56}, {24, 64.99}, {25, 63.27}, {26, 61.37}, {27, 59.22}, {28, 56.81}, {29, 54.54}, {30, 51.66}, {31, 48.88}, {32, 45.69}};
    m_vmafLut["H.264"][{1280, 720}] = {{18, 86.96}, {19, 86.15}, {20, 85.32}, {21, 84.36}, {22, 83.33}, {23, 82.09}, {24, 80.78}, {25, 79.31}, {26, 77.65}, {27, 75.87}, {28, 73.78}, {29, 71.43}, {30, 69.09}, {31, 66.30}, {32, 63.42}};
    m_vmafLut["H.264"][{1920, 1080}] = {{18, 96.48}, {19, 95.82}, {20, 95.12}, {21, 94.31}, {22, 93.46}, {23, 92.47}, {24, 91.39}, {25, 90.20}, {26, 88.92}, {27, 87.40}, {28, 85.76}, {29, 83.81}, {30, 81.76}, {31, 79.56}, {32, 77.01}};
    m_vmafLut["H.264"][{2560, 1440}] = {{18, 99.27}, {19, 99.05}, {20, 98.77}, {21, 98.34}, {22, 97.73}, {23, 96.97}, {24, 96.10}, {25, 95.06}, {26, 93.96}, {27, 92.68}, {28, 91.24}, {29, 89.70}, {30, 87.98}, {31, 85.98}, {32, 83.76}};
    // H.265
    m_vmafLut["H.265"][{854, 480}] = {{18, 72.93}, {19, 72.10}, {20, 71.18}, {21, 70.18}, {22, 69.08}, {23, 67.93}, {24, 66.53}, {25, 65.02}, {26, 63.32}, {27, 61.46}, {28, 59.46}, {29, 57.17}, {30, 54.62}, {31, 52.00}, {32, 49.16}};
    m_vmafLut["H.265"][{1280, 720}] = {{18, 87.45}, {19, 86.76}, {20, 86.07}, {21, 85.24}, {22, 84.37}, {23, 83.42}, {24, 82.32}, {25, 81.10}, {26, 79.72}, {27, 78.25}, {28, 76.41}, {29, 74.60}, {30, 72.42}, {31, 70.03}, {32, 67.31}};
    m_vmafLut["H.265"][{1920, 1080}] = {{18, 96.78}, {19, 96.23}, {20, 95.60}, {21, 94.92}, {22, 94.16}, {23, 93.33}, {24, 92.44}, {25, 91.46}, {26, 90.37}, {27, 89.12}, {28, 87.75}, {29, 86.20}, {30, 84.52}, {31, 82.46}, {32, 80.35}};
    m_vmafLut["H.265"][{2560, 1440}] = {{18, 99.44}, {19, 99.27}, {20, 99.03}, {21, 98.71}, {22, 98.25}, {23, 97.66}, {24, 96.96}, {25, 96.17}, {26, 95.25}, {27, 94.23}, {28, 93.10}, {29, 91.80}, {30, 90.41}, {31, 88.80}, {32, 86.94}};

    // VP9
    m_vmafLut["VP9"][{854, 480}] = {{18, 73.65}, {19, 73.05}, {20, 71.92}, {21, 70.77}, {22, 69.55}, {23, 68.01}, {24, 66.44}, {25, 64.41}, {26, 62.22}, {27, 59.84}, {28, 57.37}, {29, 54.58}, {30, 51.56}, {31, 48.51}, {32, 45.16}};
    m_vmafLut["VP9"][{1280, 720}] = {{18, 88.30}, {19, 87.80}, {20, 86.84}, {21, 85.95}, {22, 85.08}, {23, 83.84}, {24, 82.60}, {25, 81.18}, {26, 79.48}, {27, 77.44}, {28, 75.32}, {29, 73.23}, {30, 70.69}, {31, 67.52}, {32, 64.36}};
    m_vmafLut["VP9"][{1920, 1080}] = {{18, 97.51}, {19, 97.14}, {20, 96.42}, {21, 95.63}, {22, 94.95}, {23, 93.88}, {24, 92.94}, {25, 91.75}, {26, 90.44}, {27, 89.10}, {28, 87.26}, {29, 85.63}, {30, 83.54}, {31, 81.40}, {32, 78.52}};
    m_vmafLut["VP9"][{2560, 1440}] = {{18, 99.53}, {19, 99.44}, {20, 99.25}, {21, 98.96}, {22, 98.59}, {23, 97.90}, {24, 97.18}, {25, 96.23}, {26, 95.24}, {27, 94.06}, {28, 92.76}, {29, 91.33}, {30, 89.89}, {31, 88.09}, {32, 85.94}};

    // AV1
    m_vmafLut["AV1"][{854, 480}] = {{18, 72.60}, {19, 72.11}, {20, 71.28}, {21, 70.56}, {22, 69.90}, {23, 68.89}, {24, 67.81}, {25, 66.53}, {26, 65.45}, {27, 64.32}, {28, 63.01}, {29, 62.45}, {30, 61.60}, {31, 60.96}, {32, 60.49}};
    m_vmafLut["AV1"][{1280, 720}] = {{18, 86.85}, {19, 86.40}, {20, 85.73}, {21, 85.12}, {22, 84.53}, {23, 83.81}, {24, 82.77}, {25, 81.83}, {26, 80.70}, {27, 79.76}, {28, 78.24}, {29, 78.24}, {30, 77.29}, {31, 77.29}, {32, 76.57}};
    m_vmafLut["AV1"][{1920, 1080}] = {{18, 96.27}, {19, 95.89}, {20, 95.09}, {21, 94.53}, {22, 93.97}, {23, 92.77}, {24, 91.63}, {25, 90.44}, {26, 90.06}, {27, 89.65}, {28, 88.86}, {29, 88.86}, {30, 87.99}, {31, 87.99}, {32, 87.53}};
    m_vmafLut["AV1"][{2560, 1440}] = {{18, 99.33}, {19, 99.19}, {20, 98.82}, {21, 98.37}, {22, 97.89}, {23, 96.82}, {24, 95.70}, {25, 95.24}, {26, 95.00}, {27, 94.56}, {28, 93.51}, {29, 93.10}, {30, 92.35}, {31, 92.35}, {32, 91.90}};

}


// VMAF 查询函数的具体实现
double YtyServer::GetVmafForParams(const std::string& codec, int width, int height, int crf)
{
    auto it_codec = m_vmafLut.find(codec);
    if (it_codec != m_vmafLut.end()) {
        auto it_res = it_codec->second.find({width, height});
        if (it_res != it_codec->second.end()) {
            auto it_crf = it_res->second.find(crf);
            if (it_crf != it_res->second.end()) {
                return it_crf->second;
            }
        }
    }
    // 如果找不到精确匹配，可以返回一个默认值或基于插值的结果
    // 为简单起见，我们返回一个中等质量的值
    return 80.0;
}


// 实现客户端信息注册方法
void YtyServer::RegisterClientInfo(const Ipv4Address& clientIp, const ClientInfo& info)
{
    NS_LOG_FUNCTION(this << clientIp << info.accessType << info.region);
    m_clientInfoRegistry[clientIp] = info;
    NS_LOG_INFO("Registered: IP=" << clientIp 
                << ", CamID=" << info.cameraId 
                << ", Type=" << info.accessType
                << ", Region=" << info.region
                << ", Codec=" << info.codec);
}

void YtyServer::SetTotalBandwidth(DataRate totalBandwidth)
{
    // 这个函数会被仿真脚本(simple_network.cc) 周期性调用
    m_totalOracleBandwidth = totalBandwidth;
}


void YtyServer::StartApplication(void)
{
    // 在启动时重新检查并初始化ZMQ上下文
    if ((m_useAI || m_useUniQ) && !m_zmq_context) {
        m_zmq_context = std::make_unique<zmq::context_t>(1);
        NS_LOG_INFO("开启了AI通信");
    } else {
        NS_LOG_INFO("没有开启AI通信，使用GCC");
    }


    // Oracle 模式初始化
    if (m_useOracle)
    {
        NS_LOG_UNCOND("Oracle mode enabled. Calculating codec weights...");
        
        // 1. 定义你需求的权重比例
        m_codecWeights["H.264"] = 2;
        m_codecWeights["H.265"] = 1.3;
        m_codecWeights["VP9"]   = 1.3;
        m_codecWeights["AV1"]   = 1;

        m_codecCounts.clear();
        m_totalCodecWeight = 0.0;

        // 2. 遍历所有已注册的客户端，累加总权重
        // (这依赖于 RegisterClientInfo 必须在 StartApplication 之前被调用，
        //  在你的仿真脚本中是满足这个条件的)
        for (auto const& [ip, info] : m_clientInfoRegistry)
        {
            std::string codec = info.codec;
            if (m_codecWeights.count(codec))
            {
                m_totalCodecWeight += m_codecWeights[codec];
                m_codecCounts[codec]++;
            }
            else
            {
                // 备用：如果codec未知 (例如 "Unknown")，给一个默认权重1.0
                m_totalCodecWeight += 1.0;
                m_codecCounts["Unknown"]++;
                NS_LOG_WARN("Oracle: Unknown codec type '" << codec << "' for CamID " << info.cameraId << ". Using default weight 1.0.");
            }
        }

        NS_LOG_UNCOND("Oracle: Total clients = " << m_clientInfoRegistry.size()
                    << ", Total calculated weight = " << m_totalCodecWeight);
    }
    // Oracle 初始化结束


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

    m_logFile.open(m_logFileName, std::ios::out | std::ios::trunc);
    if (m_logFile.is_open())
    {
        m_logFile << "Time(s)\tClientAddr\tCameraId\tThroughput(kbps)\tAvgDelay(ms)\tAvgLossRate\tAvgJitter(ms)\tPlayedFrames\tStutterEvents\tStutterRate\tAccessType\tRegion\tCodec\tAvgAIBandwidth(kbps)\tAvgActualBitrate(kbps)\tAvgVMAF\tAvgCRF\tResolution\tEncoding_fps" << std::endl;
    }

    if (m_traceCameraId > 0)
    {
        // 这条日志会直接打印到你的终端，告诉我们程序收到了要追踪的ID
        NS_LOG_UNCOND("Server received request to trace Camera ID: " << m_traceCameraId);
        
        std::string traceLogFileName = "scratch/congestion_trace_cam_" + std::to_string(m_traceCameraId) + ".txt";
        m_traceLogFile.open(traceLogFileName, std::ios::out | std::ios::trunc);
        
        if (m_traceLogFile.is_open())
        {
            // 如果文件创建成功，也会在终端打印这条信息
            NS_LOG_UNCOND("Successfully created trace log file at: " << traceLogFileName);
            m_traceLogFile << "Time(s)\tIn_Throughput(kbps)\tIn_AvgDelay(ms)\tIn_LossRate\tIn_Weight\tLossDecision\tDelayDecision\tState\tOut_TargetBitrate(kbps)" << std::endl;
        }
        else
        {
            // 如果文件创建失败，会打印一条明确的错误信息
            NS_LOG_ERROR("Failed to open trace log file: " << traceLogFileName);
        }
    }
}
    

void YtyServer::StopApplication(void)
{
    for (auto const& [addr, session] : m_sessions)
    {
        Simulator::Cancel(session.reportEvent);
        Simulator::Cancel(session.playbackEvent);
        Simulator::Cancel(session.stutterTimeoutEvent);
        Simulator::Cancel(session.logStatsEvent);
    }

    m_zmq_sockets.clear(); 
    if(m_zmq_context) {
        m_zmq_context->close();
    }

    m_sessions.clear(); 

    if (m_logFile.is_open())
    {
        m_logFile.flush(); 
        m_logFile.close();
    }

    if (m_traceLogFile.is_open())
    {
        m_traceLogFile.close();
    }

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
        // 如果是空包则跳过
        if (packet->GetSize() == 0) continue;

        // --- [最终的核心修正] ---
        // 我们需要一种方法来区分RTP包和文本控制包。
        // 之前的方法（在副本上调用RemoveHeader）仍然不安全，因为它在检查之前就尝试反序列化。
        // 正确且安全的方法是只读取第一个字节，检查它是否是我们的RTP魔数(0xAC)。

        // 检查包的大小是否至少为1字节，以安全地读取第一个字节
        if (packet->GetSize() >= 1)
        {
            uint8_t magicNumber;
            // CopyData是安全的，它只复制指定数量的字节到缓冲区，不会修改原始包
            packet->CopyData(&magicNumber, 1);

            // 检查这个字节是否是RTP包的魔数
            if (magicNumber == 0xAC)
            {
                // 魔数匹配，这几乎可以肯定是我们的RTP包。
                // 现在我们可以安全地把它交给RTP处理器，它会在内部调用RemoveHeader。
                ProcessRtp(packet, from);
            }
            else
            {
                // 这是一个更健壮和带有诊断功能的文本消息处理逻辑块

                // 从数据包中安全地读取数据到缓冲区
                uint32_t packetSize = packet->GetSize();
                // 我们创建一个大小正好的char数组，而不是固定的256字节
                std::vector<char> buffer(packetSize + 1, '\0'); 
                packet->CopyData(reinterpret_cast<uint8_t*>(buffer.data()), packetSize);
                
                // 从缓冲区创建字符串
                std::string request(buffer.data(), packetSize);

                // 根据明确的字符串前缀进行判断和处理
                if (request.rfind("PLAY", 0) == 0 || request.rfind("TEARDOWN", 0) == 0)
                {
                    ProcessRtsp(packet, from);
                }
                else if (request.rfind("SET_PARAMS", 0) == 0)
                {
                    if (m_sessions.count(from)) {
                        ClientSession& session = m_sessions[from];
                        std::istringstream requestStream(request);
                        std::string line;
                        
                        std::string new_resolution = session.resolution;
                        uint32_t new_crf = session.crf;
                        uint32_t new_frame_rate = session.frameRate;
                        uint32_t new_actual_bitrate = session.actualBitrate;

                        while (std::getline(requestStream, line))
                        {
                            if (!line.empty() && line.back() == '\r') {
                                line.pop_back();
                            }
                            std::string header_res = "X-Resolution: ";
                            std::string header_crf = "X-CRF: ";
                            std::string header_fr = "X-Frame-Rate: ";
                            std::string header_br = "X-Actual-Bitrate: ";

                            if (line.rfind(header_res, 0) == 0) {
                                new_resolution = line.substr(header_res.length());
                            }
                            else if (line.rfind(header_crf, 0) == 0) {
                                new_crf = std::stoul(line.substr(header_crf.length()));
                            }
                            else if (line.rfind(header_fr, 0) == 0) {
                                new_frame_rate = std::stoul(line.substr(header_fr.length()));
                            }
                            else if (line.rfind(header_br, 0) == 0) {
                                new_actual_bitrate = std::stoul(line.substr(header_br.length()));
                            }
                        }

                        // 检查帧率是否真的发生了变化
                        if (session.frameRate != new_frame_rate)
                        {
                            NS_LOG_INFO("At time " << Simulator::Now().GetSeconds() 
                                        << "s, Server detected FrameRate change for Camera " << session.clientInfo.cameraId
                                        << " from " << session.frameRate << " to " << new_frame_rate << ". Rescheduling playback.");
                            
                            // 1. 更新会话中存储的帧率
                            session.frameRate = new_frame_rate;

                            // 2. 根据新帧率重新计算合理的卡顿超时时间
                            if (new_frame_rate > 0) {
                                session.stutterTimeout = MilliSeconds(1500.0 / new_frame_rate);
                            }

                            // 3. (最重要) 取消当前正在等待的播放事件，并立即用新的帧率重新安排播放
                            if (session.playbackEvent.IsPending()) {
                                Simulator::Cancel(session.playbackEvent);
                            }
                            // 立即用新的帧率启动下一次播放调度
                            SchedulePlayback(from); 
                        }

                        session.resolution = new_resolution;
                        session.crf = new_crf;
                        session.actualBitrate = new_actual_bitrate;

                        session.logIntervalSumActualBitrateBps += session.actualBitrate;
                        // 累加 CRF 和 FrameRate
                        session.logIntervalSumCrf += session.crf;
                        session.logIntervalSumFrameRate += session.frameRate; // 注意这里用的是当前协商的 frameRate
                        
                        // 将分辨率插入集合（std::set 会自动处理去重）
                        session.logIntervalResolutions.insert(session.resolution);

                        int width = 0, height = 0;
                        size_t x_pos = session.resolution.find('x');
                        if (x_pos != std::string::npos) {
                            try {
                                width = std::stoi(session.resolution.substr(0, x_pos));
                                height = std::stoi(session.resolution.substr(x_pos + 1));
                            } catch (const std::exception& e) {
                                width = 0; height = 0;
                            }
                        }
                        if (width > 0 && height > 0) {
                            double currentVMAF = GetVmafForParams(session.clientInfo.codec, width, height, session.crf);
                            session.logIntervalSumVmaf += currentVMAF;
                            session.lastVMAF = currentVMAF;
                        }

                        session.logIntervalParamUpdateCount++;
                    }
                }
                else
                {
                    NS_LOG_WARN("The received text packet did not match any known commands (PLAY, TEARDOWN, SET_PARAMS).");
                }
            }
        }
        else
        {
            // 包的大小甚至小于1字节，这不太可能发生，但作为健壮性检查，我们将其视为未知包。
            NS_LOG_WARN("收到一个大小小于1字节的包，已忽略。");
        }
    }
}


void YtyServer::ProcessRtp(Ptr<Packet> packet, const Address& from)
{
    // 如果会话还没有通过 PLAY 请求启动，则忽略数据包
    if (m_sessions.find(from) == m_sessions.end())
    {
        return;
    }
    
    ClientSession& session = m_sessions[from];

    // 创建一个副本用于读取头，因为 RemoveHeader 会修改原始包
    Ptr<Packet> packetCopy = packet->Copy();
    RtpHeader rtpHeader;
    packetCopy->RemoveHeader(rtpHeader);

    // 在缓冲数据包之前，检查它是否已经过时。
    uint32_t frameSeq = rtpHeader.GetFrameSeq();
    if (frameSeq < session.nextFrameToPlay)
    {
        return; // 丢弃过时的包，函数直接返回
    }


    session.hasReceivedAPacket = true;

    // 这确保了日志只在数据真实流动后才开始，消除了初始的零值垃圾数据。
    if (!session.loggingStarted)
    {
        // NS_LOG_INFO("First RTP packet received from " << InetSocketAddress::ConvertFrom(from).GetIpv4() << ". Starting periodic logging for this session.");

        // +++ 当日志首次启动时，记录当前时间作为日志周期的起点 +++
        session.logIntervalStartTime = Simulator::Now();

        ScheduleLog(from);          // 启动日志记录循环
        session.loggingStarted = true; // 设置标志，防止重复启动
    }

    uint32_t packetSize = packet->GetSize();
    Time now = Simulator::Now();

    Time sentTime = NanoSeconds(rtpHeader.GetTimestamp());
    Time delay = now - sentTime;
    
    // 更新网络统计
    session.intervalReceivedPackets++;
    session.intervalReceivedBytes += packetSize;
    session.intervalTotalDelay += delay;

    session.logIntervalReceivedBytes += packetSize;

    uint32_t cumulativeSentCount = rtpHeader.GetTotalPackets();
    if (cumulativeSentCount > session.maxSeenSentPackets) {
        session.maxSeenSentPackets = cumulativeSentCount;
    }

    // --- VVV 新增：抖动计算逻辑 (基于 RFC 3550) VVV ---
    if (!session.lastArrivalTime.IsZero()) {
        Time transit = now - sentTime;
        Time lastTransit = session.lastArrivalTime - session.lastSentTime;
        
        int64_t diff_ns = std::abs(transit.GetNanoSeconds() - lastTransit.GetNanoSeconds());
        double diff_s = diff_ns / 1e9; // 转换为秒

        // 使用平滑算法更新抖动: J = J + (|D| - J) / 16
        session.jitter += (diff_s - session.jitter) / 16.0;
    }
    session.lastSentTime = sentTime;
    session.lastArrivalTime = now;
    // --- ^^^ 新增 ^^^ ---

    // --- 新的抖动缓冲逻辑 ---
    uint32_t packetSeq = rtpHeader.GetPacketSeq();

    // 将数据包存入抖动缓冲区
    session.buffer[frameSeq][packetSeq] = {packet, now};
    // 打印服务器缓冲区日志
    // NS_LOG_INFO("At time " << now.GetSeconds() << "s, Server buffered packet for frame " << frameSeq << ", packet " << packetSeq);

    // 如果这就是我们当前正在等待的帧，立即尝试播放它
    if(frameSeq == session.nextFrameToPlay)
    {
        TryPlayback(from);
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
        Ipv4Address clientIp = InetSocketAddress::ConvertFrom(from).GetIpv4();

        // --- 【最终简化逻辑】 ---
        // 如果会话已经存在，那么这个 PLAY 请求就是一个（现在我们已决定忽略的）重协商请求。
        // 直接返回，不做任何处理，不重置会话，不打断播放。
        if (m_sessions.count(from))
        {
            NS_LOG_INFO("At time " << Simulator::Now().GetSeconds() << "s, Server ignored subsequent PLAY request from existing session: " << clientIp);
            return; // 直接返回，忽略该请求
        }

        // 只有在会话不存在时（即第一次收到PLAY请求），才创建新会话。
        NS_LOG_INFO("At time " << Simulator::Now().GetSeconds() << "s, Server received initial PLAY request from " << clientIp << ". Creating new session.");
        m_sessions[from] = ClientSession();
        ClientSession& session = m_sessions[from];

        // --- 使用固定的播放参数，不再解析请求 ---
        const uint32_t FIXED_FRAME_RATE = 30;
        session.frameRate = FIXED_FRAME_RATE;
        // 根据固定帧率计算一个合理的卡顿超时（例如1.5倍帧间隔）
        session.stutterTimeout = MilliSeconds(1500.0 / FIXED_FRAME_RATE);
        NS_LOG_INFO("Session for " << clientIp << " created with fixed playback rate: 30 fps.");


        // --- 原有的会话初始化代码（保持不变） ---
        session.lastReportTime = Simulator::Now();
        auto it = m_clientInfoRegistry.find(clientIp);
        if (it == m_clientInfoRegistry.end())
        {
            NS_LOG_WARN("Server received PLAY from an unregistered IP: " << clientIp << ". Ignoring.");
            m_sessions.erase(from); // 创建了就删掉
            return;
        }
        session.clientInfo = it->second;
        session.lastThroughputKbpsForAI = 2000.0; // 给予一个初始带宽

        // --- 解析Codec（这个可以保留，因为它不影响播放节奏） ---
        std::string codec_header_key = "X-Codec: ";
        size_t codec_pos = request.find(codec_header_key);
        if (codec_pos != std::string::npos)
        {
            size_t end_pos = request.find("\r\n", codec_pos);
            session.clientInfo.codec = request.substr(codec_pos + codec_header_key.length(), end_pos - (codec_pos + codec_header_key.length()));
        }

        // 启动播放和统计报告
        SchedulePlayback(from);
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


    // 检查在这个报告周期内是否收到了任何数据包。
    // `session.hasReceivedAPacket` 是一个保护条件，确保此逻辑只在会话正常开始后才触发，避免在仿真刚开始、第一个包还没到时就误判为宕机。
    if (session.hasReceivedAPacket && session.intervalReceivedPackets == 0)
    {
        // 打印警告日志，方便调试，确认恢复逻辑已被触发
        NS_LOG_WARN("At time " << now.GetSeconds() 
                      << "s, Camera " << session.clientInfo.cameraId 
                      << " appears stalled (no packets in " << interval.GetMilliSeconds() 
                      << "ms). Sending a low probing bitrate to force recovery.");

        // 1. 定义一个极低的“探测码率”。
        //    这个值（200kbps）低于H.264数据库中的最低码率（187kbps）,这将强制摄像头的CodecSimulator通过其降级逻辑，选择一个绝对可行的最低配置来恢复视频流。
        uint32_t probingBitrateBps = 200000; // 150 kbps

        // // 关键一步：告诉 GCC 现在的码率已经是 200kbps 了，别再做梦了
        // session.gccController->ResetState(probingBitrateBps / 1000.0);

        // 2. 直接打包并发送这个探测码率，主动引导摄像头恢复。
        Ptr<Packet> rtcpPacket = Create<Packet>(reinterpret_cast<const uint8_t*>(&probingBitrateBps), sizeof(uint32_t));
        m_socket->SendTo(rtcpPacket, 0, clientAddress);
        
        // 3. 更新统计值，以便在日志中记录这次探测行为。
        session.logIntervalSumAiBandwidthBps += probingBitrateBps;
        session.logIntervalRtcpCount++;

        // 4. 重置周期统计数据，为下一次真实的统计做准备。
        session.intervalReceivedBytes = 0;
        session.intervalTotalDelay = Seconds(0);
        session.lastReportTime = now;
        session.lastReportedSentPackets = session.maxSeenSentPackets;

        // 5. 重新安排下一次报告，并【立即返回】，跳过下面所有基于错误输入的GCC计算。
        ScheduleReport(clientAddress);
        return;
    }

  
    // 1. 计算在这个统计周期内，发送端总共发送了多少个包。
    //    maxSeenSentPackets 是这个周期内收到的RTP包头里最大的全局序号。
    //    lastReportedSentPackets 是上个周期记录的最大全局序号。
    //    它们的差值，就是这个周期内发送端发出的总包数。
    uint32_t intervalSent = session.maxSeenSentPackets - session.lastReportedSentPackets;

    double lossRate = 0.0;
    if (intervalSent > 0)
    {
        // intervalReceivedPackets 是这个周期内实际收到的总包数。
        // 这个值是通过在 ProcessRtp 中对每个到达的包计数得来的，是绝对准确的。
        lossRate = 1.0 - (double)session.intervalReceivedPackets / intervalSent;
    }
    // 安全检查，确保丢包率不会是负数（可能由于乱序导致 maxSeenSentPackets 更新延迟）
    if (lossRate < 0) lossRate = 0.0;
    
    Time avgDelay = (session.intervalReceivedPackets > 0) ? session.intervalTotalDelay / session.intervalReceivedPackets : Seconds(0);

    // (*** 注意 ***: 这里可以选择用1s均值还是50ms的瞬时值)
    double bandwidthToReportKbps = session.lastThroughputKbpsForAI; // (从1s循环更新)

    // 用50ms的瞬时值来更新给GCC的吞吐量
    // if (session.intervalReceivedPackets > 0) { // (用50ms的瞬时值覆盖，如果存在)
    //      bandwidthToReportKbps = (session.intervalReceivedBytes * 8.0) / interval.GetSeconds() / 1000.0;
    // }


    uint32_t targetBitrateBps;
    std::string loss_decision = "N/A";  // 初始化为"N/A"，适用于AI模式
    std::string delay_decision = "N/A"; // 初始化为"N/A"，适用于AI模式
    std::string state;

    // 1: 收集状态样本
    // (确保 session.jitter 已经在 ProcessRtp 中计算)
    double jitterMs = session.jitter * 1000.0; // 从秒转换为毫秒
    session.metricSamples.push_back({
        bandwidthToReportKbps,
        avgDelay.GetMilliSeconds(),
        lossRate,
        jitterMs
    });


    // 2: 运行50ms的GCC战术循环
    // 1. 获取由1s循环设定的"战略权重"
    double weight_to_use = session.aiControlledWeight;
    
    // 2. 运行GCC，传入该权重
    GCCResult gcc_result = session.gccController->get_target_bitrate_kbps(
                                bandwidthToReportKbps,
                                avgDelay.GetMilliSeconds(),
                                lossRate,
                                avgDelay.GetMilliSeconds(),
                                Simulator::Now().GetMilliSeconds(),
                                weight_to_use
                            );
    
    targetBitrateBps = static_cast<uint32_t>(gcc_result.target_bitrate_kbps * 1000.0);
    loss_decision = gcc_result.loss_decision;
    delay_decision = gcc_result.delay_decision;
    state = session.gccController->get_state_string();

    // 3. (兼容性) 允许Oracle模式覆盖GCC的决策
    if (m_useOracle)
    {
        state = "Oracle";
        double clientWeight = 1.0; 
        if (m_codecWeights.count(session.clientInfo.codec)) {
            clientWeight = m_codecWeights[session.clientInfo.codec];
        }
        double allocatedBps = 0.0;
        if (m_totalCodecWeight > 0) {
            allocatedBps = m_totalOracleBandwidth.GetBitRate() * (clientWeight / m_totalCodecWeight);
        }
        targetBitrateBps = static_cast<uint32_t>(allocatedBps);
        session.lastAiBitrateDecisionBps = targetBitrateBps;
    }


    // 3: 更新追踪日志
    if (m_traceCameraId > 0 && session.clientInfo.cameraId == m_traceCameraId)
    {
        if (m_traceLogFile.is_open())
        {
            m_traceLogFile << Simulator::Now().GetSeconds() << "\t"
                           << bandwidthToReportKbps << "\t"
                           << avgDelay.GetMilliSeconds() << "\t"
                           << lossRate << "\t"
                           << weight_to_use << "\t" // <-- 记录AI/Minerva设定的权重
                           << (m_useOracle ? "N/A" : loss_decision) << "\t"
                           << (m_useOracle ? "N/A" : delay_decision) << "\t"
                           << state << "\t"
                           << targetBitrateBps / 1000.0 << std::endl;
        }
    }

    session.aiBandwidth = targetBitrateBps; // 更新用于日志的aiBandwidth字段
    session.logIntervalSumAiBandwidthBps += targetBitrateBps; 

    Ptr<Packet> rtcpPacket = Create<Packet>(reinterpret_cast<const uint8_t*>(&targetBitrateBps), sizeof(uint32_t));
    m_socket->SendTo(rtcpPacket, 0, clientAddress);

    // --- 重置50ms周期的统计数据 ---
    // (*** 注意 ***: 我们需要将50ms周期的统计累加到1s周期的累加器中)
    session.logIntervalSumDelay += avgDelay;
    session.logIntervalSumLossRate += lossRate;
    session.logIntervalSumJitter += session.jitter; // (LogPlaybackStats会*1000)
    session.logIntervalRtcpCount++;

    // (重置50ms的瞬时计数器)
    session.intervalReceivedPackets = 0;
    session.intervalReceivedBytes = 0;
    session.intervalTotalDelay = Seconds(0);
    session.lastReportTime = now;
    session.lastReportedSentPackets = session.maxSeenSentPackets;

    ScheduleReport(clientAddress);
}



// --- 新的播放和日志记录函数 ---

void YtyServer::SchedulePlayback(const Address& clientAddress)
{
    if (m_sessions.count(clientAddress)) {
        // 使用会话中存储的、协商好的帧率
        ClientSession& session = m_sessions[clientAddress];
        if (session.frameRate == 0) return; // 防止除以0

        Time playbackInterval = Seconds(1.0 / session.frameRate);
        m_sessions[clientAddress].playbackEvent = Simulator::Schedule(playbackInterval, &YtyServer::TryPlayback, this, clientAddress);
    }
}

void YtyServer::TryPlayback(const Address& clientAddress)
{
    if (!m_sessions.count(clientAddress)) return;

    ClientSession& session = m_sessions[clientAddress];
    uint32_t frameToPlay = session.nextFrameToPlay;
    
    // 检查帧是否存在于缓冲区中
    auto it = session.buffer.find(frameToPlay);
    if (it == session.buffer.end())
    {
        // 帧完全不存在。安排一个卡顿超时。
        if (!session.stutterTimeoutEvent.IsPending()) {
             session.stutterTimeoutEvent = Simulator::Schedule(session.stutterTimeout, &YtyServer::HandleStutter, this, clientAddress);
        }
        return; // 等待数据包或卡顿超时
    }

    // 帧存在，检查它是否完整。
    // 为此，我们需要知道这一帧总共有多少包。
    // 我们可以查看我们收到的该帧第一个包的头部信息。
    auto& packetsInFrameMap = it->second;
    Ptr<Packet> firstPacket = packetsInFrameMap.begin()->second.packet->Copy();
    RtpHeader header;
    firstPacket->RemoveHeader(header);
    uint32_t requiredPackets = header.GetPacketsInFrame();

    if (packetsInFrameMap.size() >= requiredPackets)
    {
        // 帧是完整的！
        NS_LOG_INFO("At time " << Simulator::Now().GetSeconds() << "s, Server PLAYED frame " << frameToPlay);
        
        // 取消为该帧设置的任何卡顿超时，因为它现在已经到达。
        if (session.stutterTimeoutEvent.IsPending()) {
            Simulator::Cancel(session.stutterTimeoutEvent);
        }

        session.playedFrames++;
        session.nextFrameToPlay++; // 移动到下一帧
        session.buffer.erase(frameToPlay); // 清理缓冲区

        // 安排下一次播放尝试。
        SchedulePlayback(clientAddress);
    }
    else
    {
        // 帧已开始到达但尚不完整。如果卡顿计时器还未运行，则启动它。
        if (!session.stutterTimeoutEvent.IsPending()) {
             session.stutterTimeoutEvent = Simulator::Schedule(session.stutterTimeout, &YtyServer::HandleStutter, this, clientAddress);
        }
    }
}

void YtyServer::HandleStutter(const Address& clientAddress)
{
    if (!m_sessions.count(clientAddress)) return;

    ClientSession& session = m_sessions[clientAddress];
    NS_LOG_INFO("At time " << Simulator::Now().GetSeconds() << "s, STUTTER detected for frame " << session.nextFrameToPlay << ". Skipping.");

    
    // 在跳过这一帧之前，必须将其已缓存的数据从抖动缓冲区中清除。这是防止数据结构无限增长、导致仿真速度变慢的关键。
    uint32_t frameToClean = session.nextFrameToPlay;
    if (session.buffer.count(frameToClean))
    {   
        // 从 map 中删除这个永远不会被播放的帧的所有相关数据。
        session.buffer.erase(frameToClean);
        NS_LOG_INFO("从缓冲区中删掉帧 " << frameToClean << " 避免持续累积.");
    }

    session.stutterEvents++;
    session.nextFrameToPlay++; // 跳过迟到的帧

    // 跳过之后，立即尝试播放下一帧。
    SchedulePlayback(clientAddress);
}


void YtyServer::ScheduleLog(const Address& clientAddress)
{
    if (m_sessions.count(clientAddress)) {
        m_sessions[clientAddress].logStatsEvent = Simulator::Schedule(m_logInterval, &YtyServer::LogPlaybackStats, this, clientAddress);
    }
}


void YtyServer::LogPlaybackStats(const Address& clientAddress)
{
    if (!m_sessions.count(clientAddress)) return;

    ClientSession& session = m_sessions[clientAddress];
    
    // --- 计算播放统计 (保持不变) ---
    double stutterRate = 0;
    if ((session.playedFrames + session.stutterEvents) > 0)
    {
        stutterRate = static_cast<double>(session.stutterEvents) / (session.playedFrames + session.stutterEvents);
    }

    // --- 计算1秒日志周期的各项指标 (保持不变) ---
    Time logIntervalDuration = Simulator::Now() - session.logIntervalStartTime;
    double throughputKbps = 0.0;
    if (logIntervalDuration.GetSeconds() > 0)
    {
        throughputKbps = (session.logIntervalReceivedBytes * 8.0) / logIntervalDuration.GetSeconds() / 1000.0;
    }
    session.lastThroughputKbpsForAI = throughputKbps; // <-- 更新吞吐量

    double avgDelayMs = 0.0;
    double avgLossRate = 0.0;
    double avgJitterMs = 0.0;
    if (session.logIntervalRtcpCount > 0)
    {
        avgDelayMs = (session.logIntervalSumDelay.GetMilliSeconds()) / session.logIntervalRtcpCount;
        avgLossRate = session.logIntervalSumLossRate / session.logIntervalRtcpCount;
        avgJitterMs = (session.logIntervalSumJitter / session.logIntervalRtcpCount) * 1000.0; // 转换为毫秒
    }
    
    double avgAiBandwidthKbps = 0.0;
    if (session.logIntervalRtcpCount > 0) {
        avgAiBandwidthKbps = (session.logIntervalSumAiBandwidthBps / session.logIntervalRtcpCount) / 1000.0;
    }

    double avgActualBitrateKbps = 0.0;
    double avgVmaf = 0.0;
    double avgCrf = 0.0;
    double avgEncodingFps = 0.0;
    std::string resolutionStr = "";

    if (session.logIntervalParamUpdateCount > 0) {
        avgActualBitrateKbps = (session.logIntervalSumActualBitrateBps / session.logIntervalParamUpdateCount) / 1000.0;
        avgVmaf = session.logIntervalSumVmaf / session.logIntervalParamUpdateCount;

        // 计算平均 CRF
        avgCrf = session.logIntervalSumCrf / session.logIntervalParamUpdateCount;
        // 计算平均 Encoding FPS
        avgEncodingFps = session.logIntervalSumFrameRate / session.logIntervalParamUpdateCount;

        // 拼接分辨率字符串 (例如: "854x480,1280x720")
        for (auto it = session.logIntervalResolutions.begin(); it != session.logIntervalResolutions.end(); ++it) {
            if (it != session.logIntervalResolutions.begin()) {
                resolutionStr += ",";
            }
            resolutionStr += *it;
        }
    }
    else 
    {
        avgVmaf = session.lastVMAF;
        avgCrf = static_cast<double>(session.crf);
        avgEncodingFps = static_cast<double>(session.frameRate);
        resolutionStr = session.resolution;

    }
    
    
    // --- 1. 计算奖励 (Reward) ---
    // (您现有的Minerva QoE计算逻辑)
    double vmafJitter = std::abs(avgVmaf - session.lastVMAF); // <-- 计算 vmafJitter
    double reward = avgVmaf - 75.0 * stutterRate - 2.5 * vmafJitter;
    session.qoeValue = reward;
    session.lastVMAF = avgVmaf; 

    // --- 2. 聚合状态 (State) ---
    // (调用新的聚合函数，传入 vmafJitter)
    std::string currentStateJson = AggregateState(
        session.metricSamples, 
        avgVmaf, 
        stutterRate, 
        vmafJitter, // <-- 传入
        session.clientInfo.codec
    );
    session.metricSamples.clear(); // 清空样本

    // 判断当前摄像头ID是否在允许使用算法的范围内
    // 如果 m_algoCameraIdLimit 是默认值(最大值)，这里永远为 true
    bool allowAlgo = (session.clientInfo.cameraId <= m_algoCameraIdLimit);


    // --- 3. 运行1秒战略决策 (AI, Minerva, 或 GCC) ---
    if (m_useAI && allowAlgo)
    {
        // --- AI (RL) 决策流程 ---
        if (m_zmq_sockets.find(clientAddress) == m_zmq_sockets.end()) {
            NS_LOG_INFO("Creating new ZMQ REQ socket for client " << session.clientInfo.cameraId);
            m_zmq_sockets[clientAddress] = std::make_unique<zmq::socket_t>(*m_zmq_context, ZMQ_REQ);
            m_zmq_sockets[clientAddress]->connect("tcp://localhost:5556");
        }
        auto& socket = m_zmq_sockets[clientAddress];

        // 1. 构建ZMQ请求 (S_t, A_t, R_t+1, S_t+1)
        json request_json;
        request_json["cameraId"] = session.clientInfo.cameraId;
        request_json["last_state"] = session.lastAiStateJson;
        request_json["last_action_weight"] = session.lastAiActionWeight; // 上一秒的动作(权重)
        request_json["current_reward"] = reward;                  // 这一秒的奖励(QoE)
        request_json["current_state"] = currentStateJson;         // 这一秒的状态 (包含完整向量)
        
        std::string request_str = request_json.dump();
        
        // 2. 发送和接收
        socket->send(zmq::buffer(request_str), zmq::send_flags::none);
        zmq::message_t reply;
        
        double newWeight = 1.0; // 失败时的默认权重
        if (socket->recv(reply, zmq::recv_flags::none)) {
            std::string reply_str = reply.to_string();
            try {
                json reply_json = json::parse(reply_str);
                // *** AI Python端应返回 "targetWeight" ***
                newWeight = reply_json["targetWeight"]; 
            } catch (const std::exception& e) {
                NS_LOG_WARN("AI JSON parse error: " << e.what() << ". Using default weight 1.0.");
            }
        } else {
             NS_LOG_WARN("No reply from AI. Using default weight 1.0.");
        }

        // 3. 应用新权重 (A_t+1) 并存储状态 (S_t+1, A_t+1)
        session.aiControlledWeight = std::max(0.5, std::min(newWeight, 2.0)); // 限制范围
        session.lastAiStateJson = currentStateJson;
        session.lastAiActionWeight = session.aiControlledWeight;
    }
    else if (m_useUniQ && allowAlgo)
    {
        // 调用我们刚才写的函数
        double weight = GetWeightFromUniQ(session, 
                                          throughputKbps, 
                                          avgDelayMs, 
                                          avgLossRate, 
                                          clientAddress);
        
        // UniQ 的权重直接作为 aiControlledWeight
        // 这个权重之后会传入 GCCController::get_target_bitrate_kbps 
        // 作用于乘性减 (update_bitrate 函数中)
        session.aiControlledWeight = weight;
    }
    else if (m_useMinerva && allowAlgo)
    {
        // --- Minerva 启发式决策流程 (基于绝对的QoE公平) ---
        double referenceBitrateMbps = interpolate_qoe_to_bitrate_mbps(
            session.qoeValue, 
            g_minerva_qoe_data,     
            g_minerva_bitrate_data_mbps 
        );
        double referenceBitrateKbps = referenceBitrateMbps * 1000.0;
        
        double actualBitrateKbps = (session.logIntervalParamUpdateCount > 0) ? avgActualBitrateKbps : (session.actualBitrate / 1000.0);
        if (referenceBitrateKbps > 1.0) 
        {
            session.minervaWeight = actualBitrateKbps / referenceBitrateKbps;
        }
        else
        {
            session.minervaWeight = 1.0; 
        }


        // // Minerva新权重调整：针对 H.264 减少权重
        // if (session.clientInfo.codec == "H.264")
        // {
        //     session.minervaWeight -= 0.3;
            
        //     // 安全检查：防止权重变为负数或过小，建议设置一个下限（例如 0.1）
        //     if (session.minervaWeight < 0.5) 
        //     {
        //         session.minervaWeight = 0.5;
        //     }
        // }

        // // Minerva新权重调整：针对 VP9 减少权重
        // if (session.clientInfo.codec == "VP9")
        // {
        //     session.minervaWeight -= 0.5;
            
        //     // 安全检查：防止权重变为负数或过小，建议设置一个下限（例如 0.1）
        //     if (session.minervaWeight < 0.5) 
        //     {
        //         session.minervaWeight = 0.5;
        //     }
        // }



        const double alpha = 0.05;
        session.smoothedMinervaWeight = alpha * session.minervaWeight + (1.0 - alpha) * session.smoothedMinervaWeight;
        
        // 应用Minerva的决策
        session.aiControlledWeight = session.smoothedMinervaWeight;


        // === [修改] 模式切换：实现群体 QoE 总和最大化 (Max Sum Utility) ===
        // 理论公式: w = r * U'(r)
        // 含义: 权重 = 当前带宽 * 边际效用(斜率)

        // // 1. 获取当前实际码率 (转化为 Mbps 以匹配查找表)
        // double currentBitrateMbps = (session.logIntervalParamUpdateCount > 0) ? 
        //                             (avgActualBitrateKbps / 1000.0) : 
        //                             (session.actualBitrate / 1000.0);

        // // 2. 计算局部斜率 (Marginal Utility)
        // // 利用已有的 g_minerva_bitrate_data_mbps (X轴) 和 g_minerva_qoe_data (Y轴)
        // double slope = 0.0;
        
        // // 找到当前码率在表中的位置
        // auto it = std::lower_bound(g_minerva_bitrate_data_mbps.begin(), g_minerva_bitrate_data_mbps.end(), currentBitrateMbps);
        // size_t idx = std::distance(g_minerva_bitrate_data_mbps.begin(), it);

        // // 边界处理与差分计算
        // if (idx > 0 && idx < g_minerva_bitrate_data_mbps.size()) {
        //     double dQoE = g_minerva_qoe_data[idx] - g_minerva_qoe_data[idx - 1];
        //     double dBitrate = g_minerva_bitrate_data_mbps[idx] - g_minerva_bitrate_data_mbps[idx - 1];
        //     if (dBitrate > 1e-6) slope = dQoE / dBitrate; // 斜率 = QoE提升 / Mbps
        // } 
        // else if (idx == 0) {
        //     // 如果码率极低，使用第一段的斜率
        //     double dQoE = g_minerva_qoe_data[1] - g_minerva_qoe_data[0];
        //     double dBitrate = g_minerva_bitrate_data_mbps[1] - g_minerva_bitrate_data_mbps[0];
        //     slope = dQoE / dBitrate;
        // }
        // // 如果 idx == size，说明码率已超表，通常处于饱和区，斜率接近0，保持 slope=0 即可

        // // 3. 计算原始权重 (r * Slope)
        // // 注意：Slope 的单位是 QoE/Mbps。
        // // 低码率时斜率通常很高（例如 30），高码率时接近 0。
        // // 为了防止权重过大（超过2.0会被截断），建议加一个归一化系数。
        // // 假设我们希望平均权重维持在 1.0 左右，根据经验数据，建议除以 15.0 左右。
        // double rawWeight = currentBitrateMbps * slope; 

        // // 如果直接用斜率
        // // double rawWeight = slope; 

        // double scalingFactor = 1.0 / 20.0; // 经验归一化系数，可根据日志微调

        // session.minervaWeight = rawWeight * scalingFactor;

        // // 4. 平滑处理 (保持原样，防止权重跳变过快)
        // const double alpha = 0.1;
        // session.smoothedMinervaWeight = alpha * session.minervaWeight + (1.0 - alpha) * session.smoothedMinervaWeight;
        
        // // 应用决策
        // session.aiControlledWeight = session.smoothedMinervaWeight;
    }
    else
    {
        // --- 纯 GCC 决策流程 ---
        // 如果 allowAlgo 为 false (即 CameraId > x)，也会进入这里
        session.aiControlledWeight = 1.0; // 权重为1
    }
    
    // --- 4. 日志记录 (保持不变) ---
    if (m_logFile.is_open())
    {
        m_logFile << Simulator::Now().GetSeconds() << "\t"
                  << InetSocketAddress::ConvertFrom(clientAddress).GetIpv4() << "\t"
                  << session.clientInfo.cameraId << "\t"
                  << throughputKbps << "\t"
                  << avgDelayMs << "\t"
                  << avgLossRate << "\t"
                  << avgJitterMs << "\t"
                  << session.playedFrames << "\t"
                  << session.stutterEvents << "\t"
                  << stutterRate << "\t"
                  << session.clientInfo.accessType << "\t"
                  << session.clientInfo.region << "\t"
                  << session.clientInfo.codec << "\t"
                  << avgAiBandwidthKbps << "\t"
                  << avgActualBitrateKbps << "\t"
                  << avgVmaf << "\t"
                  << avgCrf << "\t"
                  << resolutionStr << "\t"
                  << avgEncodingFps << '\n';
    }
    
    // --- 5. 为下一个日志周期重置所有相关的统计量 (保持不变) ---
    session.playedFrames = 0;
    session.stutterEvents = 0;
    session.logIntervalSumDelay = Seconds(0);
    session.logIntervalSumLossRate = 0.0;
    session.logIntervalSumJitter = 0.0;
    session.logIntervalRtcpCount = 0;
    session.logIntervalReceivedBytes = 0;
    session.logIntervalStartTime = Simulator::Now();
    session.logIntervalSumAiBandwidthBps = 0.0;
    session.logIntervalSumActualBitrateBps = 0.0;
    session.logIntervalSumVmaf = 0.0;
    session.logIntervalParamUpdateCount = 0;
    session.logIntervalSumCrf = 0.0;
    session.logIntervalSumFrameRate = 0.0;
    session.logIntervalResolutions.clear();

    // 安排下一次日志事件
    ScheduleLog(clientAddress);
}


// 如果开启了AI模型就用AI模型，没有开启就用GCC
uint32_t YtyServer::GetTargetBitrate(ClientSession& session, double throughputKbps, Time delay, double lossRate)
{

    if (m_useOracle)
    {
        // 这部分逻辑与 SendRtcpFeedback 中完全一致
        double clientWeight = 1.0; 
        if (m_codecWeights.count(session.clientInfo.codec)) {
            clientWeight = m_codecWeights[session.clientInfo.codec];
        }
        double allocatedBps = 0.0;
        if (m_totalCodecWeight > 0) {
            allocatedBps = m_totalOracleBandwidth.GetBitRate() * (clientWeight / m_totalCodecWeight);
        }
        uint32_t targetBitrateBps = static_cast<uint32_t>(allocatedBps);
        session.lastAiBitrateDecisionBps = targetBitrateBps;
        return targetBitrateBps;
    }
    
    else if (m_useAI)
    {
        // 找到该客户端对应的地址
        Address clientAddress;
        for (auto const& [addr, s] : m_sessions) {
            // 注意：这里用clientInfo.cameraId比较，因为我们只知道session
            if (s.clientInfo.cameraId == session.clientInfo.cameraId) {
                clientAddress = addr;
                break;
            }
        }
        return GetBitrateFromAI(session, throughputKbps, delay, lossRate, clientAddress);
    }
    // 否则，使用内置的GCC算法
    else
    {
        long long current_time_ms = Simulator::Now().GetMilliSeconds();

        // 我们使用 smoothedMinervaWeight 以获得更稳定的表现
        double weight = session.smoothedMinervaWeight;

        // 1. 先用 GCCResult 类型的变量接收函数返回的结构体
        GCCResult result = session.gccController->get_target_bitrate_kbps(
                                    throughputKbps,
                                    delay.GetMilliSeconds(),
                                    lossRate,
                                    delay.GetMilliSeconds(), // RTT用延迟近似
                                    current_time_ms,
                                    weight  // 将权重传递给GCC控制器
                                );
        
        // 2. 从结构体中提取出我们需要的码率值
        double target_kbps = result.target_bitrate_kbps;
        
        session.lastAiBitrateDecisionBps = static_cast<uint32_t>(target_kbps * 1000.0);
        return session.lastAiBitrateDecisionBps;
    }
}


// 实现与AI的通信函数
uint32_t YtyServer::GetBitrateFromAI(ClientSession& session, double throughputKbps, Time delay, double lossRate, const Address& from)
{
    // 检查此客户端是否已经有ZMQ socket，如果没有则创建一个
    if (m_zmq_sockets.find(from) == m_zmq_sockets.end()) {
        NS_LOG_INFO("Creating new ZMQ REQ socket for client " << session.clientInfo.cameraId);
        m_zmq_sockets[from] = std::make_unique<zmq::socket_t>(*m_zmq_context, ZMQ_REQ);
        m_zmq_sockets[from]->connect("tcp://localhost:5556");
    }

    auto& socket = m_zmq_sockets[from];

    // 1. 构建JSON请求
    // 服务器提供的是(throughputKbps, delayMs, lossRate, 码率差)
    // 我们需要从session中获取上一次的码率来计算差值
    double last_bitrate_kbps = session.lastAiBitrateDecisionBps / 1000.0;
    double bitrate_diff_kbps = throughputKbps - last_bitrate_kbps;

    json request_json;
    request_json["cameraId"] = session.clientInfo.cameraId;
    request_json["throughputKbps"] = throughputKbps;
    request_json["delayMs"] = delay.GetMilliSeconds();
    request_json["lossRate"] = lossRate;
    request_json["bitrate_diff"] = bitrate_diff_kbps;
    request_json["codec"] = session.clientInfo.codec;
    
    std::string request_str = request_json.dump();
    
    // 2. 发送请求
    NS_LOG_INFO("To AI -> " << request_str);
    socket->send(zmq::buffer(request_str), zmq::send_flags::none);

    // 3. 等待并接收回复
    zmq::message_t reply;
    auto res = socket->recv(reply, zmq::recv_flags::none);

    if (res) {
        std::string reply_str = reply.to_string();
        NS_LOG_INFO("From AI <- " << reply_str);
        try {
            json reply_json = json::parse(reply_str);
            uint32_t target_bitrate_bps = reply_json["targetBitrate"];
            
            // 更新会话中记录的上一次决策码率
            session.lastAiBitrateDecisionBps = target_bitrate_bps;
            return target_bitrate_bps;
        } catch (const std::exception& e) {
            NS_LOG_ERROR("AI解析错误: " << e.what() << ". 使用历史码率.");
            // 如果解析失败，返回上一次的码率以保证稳定性
            return session.lastAiBitrateDecisionBps;
        }
    } else {
        NS_LOG_WARN("没有获得AI模型回复，使用历史码率.");
        return session.lastAiBitrateDecisionBps;
    }
}


// 实现 GetWeightFromUniQ
double YtyServer::GetWeightFromUniQ(ClientSession& session, double throughputKbps, double avgDelayMs, double avgLossRate, const Address& from)
{
    // 1. 初始化 Socket (复用现有逻辑)
    if (m_zmq_sockets.find(from) == m_zmq_sockets.end()) {
        NS_LOG_INFO("Creating new ZMQ REQ socket for UniQ client " << session.clientInfo.cameraId);
        m_zmq_sockets[from] = std::make_unique<zmq::socket_t>(*m_zmq_context, ZMQ_REQ);
        m_zmq_sockets[from]->connect("tcp://localhost:5556");
    }

    auto& socket = m_zmq_sockets[from];

    // 2. 构建 UniQ 专属 JSON
    // 需要: AvgActualBitrate, Pixels(Resolution), Encoding_fps, AvgCRF, Throughput, AvgDelay, AvgLossRate, Codec
    json request_json;
    request_json["cameraId"] = session.clientInfo.cameraId;
    
    // 使用 session 中统计的 1s 平均值（如果存在），否则使用当前瞬时值
    // 注意：在 LogPlaybackStats 调用此函数前，这些 Avg 值已经被计算好了
    
    // 实际上报码率
    double actualBitrateKbps = (session.logIntervalParamUpdateCount > 0) 
                               ? (session.logIntervalSumActualBitrateBps / session.logIntervalParamUpdateCount) / 1000.0 
                               : (session.actualBitrate / 1000.0);
    
    // 平均 FPS
    double avgFps = (session.logIntervalParamUpdateCount > 0)
                    ? (session.logIntervalSumFrameRate / session.logIntervalParamUpdateCount)
                    : (double)session.frameRate;

    // 平均 CRF
    double avgCrf = (session.logIntervalParamUpdateCount > 0)
                    ? (session.logIntervalSumCrf / session.logIntervalParamUpdateCount)
                    : (double)session.crf;

    request_json["AvgActualBitrate(kbps)"] = actualBitrateKbps;
    request_json["Resolution"] = session.resolution; // 字符串，Python端处理 Pixels
    request_json["Encoding_fps"] = avgFps;
    request_json["AvgCRF"] = avgCrf;
    request_json["Throughput(kbps)"] = throughputKbps;
    request_json["AvgDelay(ms)"] = avgDelayMs;
    request_json["AvgLossRate"] = avgLossRate;
    request_json["Codec"] = session.clientInfo.codec;

    std::string request_str = request_json.dump();

    // 3. 发送与接收
    // NS_LOG_INFO("To UniQ -> " << request_str); // 调试可开启
    socket->send(zmq::buffer(request_str), zmq::send_flags::none);

    zmq::message_t reply;
    // 使用阻塞接收，或者设置超时
    auto res = socket->recv(reply, zmq::recv_flags::none);

    if (res) {
        std::string reply_str = reply.to_string();
        try {
            json reply_json = json::parse(reply_str);
            double weight = reply_json["targetWeight"];
            return weight;
        } catch (const std::exception& e) {
            NS_LOG_ERROR("UniQ JSON Parse Error: " << e.what());
            return 1.0;
        }
    } else {
        NS_LOG_WARN("UniQ No Reply");
        return 1.0;
    }
}




}
// namespace ns3