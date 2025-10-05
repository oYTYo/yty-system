/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */

#include "yty-codec-simulator.h"
#include "ns3/log.h"
#include "ns3/object.h"
#include <algorithm> // for std::sort
#include <map>

namespace ns3 {

NS_LOG_COMPONENT_DEFINE("YtyCodecSimulator");
NS_OBJECT_ENSURE_REGISTERED(YtyCodecSimulator);

TypeId YtyCodecSimulator::GetTypeId(void)
{
    static TypeId tid = TypeId("ns3::YtyCodecSimulator")
        .SetParent<Object>()
        .SetGroupName("Applications")
        .AddConstructor<YtyCodecSimulator>();
    return tid;
}

YtyCodecSimulator::YtyCodecSimulator() {
    NS_LOG_FUNCTION(this);
    YtyCodecSimulator("H.264");
}


YtyCodecSimulator::YtyCodecSimulator(std::string codecType) : m_codecType(codecType)
{
    NS_LOG_FUNCTION(this << codecType);
    
    const std::vector<CodecDataEntry> fullDb = {
        // H.264 Data (30 FPS - 原有数据)
        {"854x480", 854, 480, 30, 18, 1838.0}, {"854x480", 854, 480, 30, 19, 1553.0}, {"854x480", 854, 480, 30, 20, 1320.0}, {"854x480", 854, 480, 30, 21, 1124.0}, {"854x480", 854, 480, 30, 22, 962.0}, {"854x480", 854, 480, 30, 23, 829.0}, {"854x480", 854, 480, 30, 24, 719.0}, {"854x480", 854, 480, 30, 25, 627.0}, {"854x480", 854, 480, 30, 26, 550.0}, {"854x480", 854, 480, 30, 27, 484.0}, {"854x480", 854, 480, 30, 28, 428.0}, {"854x480", 854, 480, 30, 29, 380.0}, {"854x480", 854, 480, 30, 30, 338.0}, {"854x480", 854, 480, 30, 31, 302.0}, {"854x480", 854, 480, 30, 32, 271.0},
        {"1280x720", 1280, 720, 30, 18, 4315.0}, {"1280x720", 1280, 720, 30, 19, 3513.0}, {"1280x720", 1280, 720, 30, 20, 2890.0}, {"1280x720", 1280, 720, 30, 21, 2395.0}, {"1280x720", 1280, 720, 30, 22, 2010.0}, {"1280x720", 1280, 720, 30, 23, 1705.0}, {"1280x720", 1280, 720, 30, 24, 1457.0}, {"1280x720", 1280, 720, 30, 25, 1261.0}, {"1280x720", 1280, 720, 30, 26, 1103.0}, {"1280x720", 1280, 720, 30, 27, 967.0}, {"1280x720", 1280, 720, 30, 28, 851.0}, {"1280x720", 1280, 720, 30, 29, 755.0}, {"1280x720", 1280, 720, 30, 30, 672.0}, {"1280x720", 1280, 720, 30, 31, 602.0}, {"1280x720", 1280, 720, 30, 32, 542.0},
        {"1920x1080", 1920, 1080, 30, 18, 11831.0}, {"1920x1080", 1920, 1080, 30, 19, 9367.0}, {"1920x1080", 1920, 1080, 30, 20, 7428.0}, {"1920x1080", 1920, 1080, 30, 21, 5923.0}, {"1920x1080", 1920, 1080, 30, 22, 4769.0}, {"1920x1080", 1920, 1080, 30, 23, 3901.0}, {"1920x1080", 1920, 1080, 30, 24, 3250.0}, {"1920x1080", 1920, 1080, 30, 25, 2747.0}, {"1920x1080", 1920, 1080, 30, 26, 2360.0}, {"1920x1080", 1920, 1080, 30, 27, 2045.0}, {"1920x1080", 1920, 1080, 30, 28, 1788.0}, {"1920x1080", 1920, 1080, 30, 29, 1577.0}, {"1920x1080", 1920, 1080, 30, 30, 1400.0}, {"1920x1080", 1920, 1080, 30, 31, 1254.0}, {"1920x1080", 1920, 1080, 30, 32, 1128.0},
        {"2560x1440", 2560, 1440, 30, 18, 23612.0}, {"2560x1440", 2560, 1440, 30, 19, 18932.0}, {"2560x1440", 2560, 1440, 30, 20, 15097.0}, {"2560x1440", 2560, 1440, 30, 21, 11976.0}, {"2560x1440", 2560, 1440, 30, 22, 9483.0}, {"2560x1440", 2560, 1440, 30, 23, 7555.0}, {"2560x1440", 2560, 1440, 30, 24, 6100.0}, {"2560x1440", 2560, 1440, 30, 25, 5011.0}, {"2560x1440", 2560, 1440, 30, 26, 4188.0}, {"2560x1440", 2560, 1440, 30, 27, 3576.0}, {"2560x1440", 2560, 1440, 30, 28, 3087.0}, {"2560x1440", 2560, 1440, 30, 29, 2700.0}, {"2560x1440", 2560, 1440, 30, 30, 2385.0}, {"2560x1440", 2560, 1440, 30, 31, 2124.0}, {"2560x1440", 2560, 1440, 30, 32, 1902.0},
        // H.264 Data (Variable FPS for 480p) - Only CRF=32 remains
        {"854x480", 854, 480, 5, 32, 187}, {"854x480", 854, 480, 10, 32, 205}, {"854x480", 854, 480, 15, 32, 221}, {"854x480", 854, 480, 20, 32, 227}, {"854x480", 854, 480, 25, 32, 244},

        // H.265 Data (30 FPS - 原有数据)
        {"854x480", 854, 480, 30, 18, 1515.0}, {"854x480", 854, 480, 30, 19, 1285.0}, {"854x480", 854, 480, 30, 20, 1090.0}, {"854x480", 854, 480, 30, 21, 931.0}, {"854x480", 854, 480, 30, 22, 799.0}, {"854x480", 854, 480, 30, 23, 687.0}, {"854x480", 854, 480, 30, 24, 593.0}, {"854x480", 854, 480, 30, 25, 513.0}, {"854x480", 854, 480, 30, 26, 442.0}, {"854x480", 854, 480, 30, 27, 383.0}, {"854x480", 854, 480, 30, 28, 332.0}, {"854x480", 854, 480, 30, 29, 288.0}, {"854x480", 854, 480, 30, 30, 251.0}, {"854x480", 854, 480, 30, 31, 218.0}, {"854x480", 854, 480, 30, 32, 190.0},
        {"1280x720", 1280, 720, 30, 18, 3472.0}, {"1280x720", 1280, 720, 30, 19, 2853.0}, {"1280x720", 1280, 720, 30, 20, 2348.0}, {"1280x720", 1280, 720, 30, 21, 1951.0}, {"1280x720", 1280, 720, 30, 22, 1636.0}, {"1280x720", 1280, 720, 30, 23, 1378.0}, {"1280x720", 1280, 720, 30, 24, 1169.0}, {"1280x720", 1280, 720, 30, 25, 994.0}, {"1280x720", 1280, 720, 30, 26, 848.0}, {"1280x720", 1280, 720, 30, 27, 729.0}, {"1280x720", 1280, 720, 30, 28, 627.0}, {"1280x720", 1280, 720, 30, 29, 542.0}, {"1280x720", 1280, 720, 30, 30, 469.0}, {"1280x720", 1280, 720, 30, 31, 406.0}, {"1280x720", 1280, 720, 30, 32, 351.0},
        {"1920x1080", 1920, 1080, 30, 18, 8609.0}, {"1920x1080", 1920, 1080, 30, 19, 6917.0}, {"1920x1080", 1920, 1080, 30, 20, 5528.0}, {"1920x1080", 1920, 1080, 30, 21, 4422.0}, {"1920x1080", 1920, 1080, 30, 22, 3559.0}, {"1920x1080", 1920, 1080, 30, 23, 2879.0}, {"1920x1080", 1920, 1080, 30, 24, 2348.0}, {"1920x1080", 1920, 1080, 30, 25, 1942.0}, {"1920x1080", 1920, 1080, 30, 26, 1611.0}, {"1920x1080", 1920, 1080, 30, 27, 1353.0}, {"1920x1080", 1920, 1080, 30, 28, 1148.0}, {"1920x1080", 1920, 1080, 30, 29, 980.0}, {"1920x1080", 1920, 1080, 30, 30, 843.0}, {"1920x1080", 1920, 1080, 30, 31, 727.0}, {"1920x1080", 1920, 1080, 30, 32, 629.0},
        {"2560x1440", 2560, 1440, 30, 18, 17278.0}, {"2560x1440", 2560, 1440, 30, 19, 14000.0}, {"2560x1440", 2560, 1440, 30, 20, 11235.0}, {"2560x1440", 2560, 1440, 30, 21, 8946.0}, {"2560x1440", 2560, 1440, 30, 22, 7100.0}, {"2560x1440", 2560, 1440, 30, 23, 5613.0}, {"2560x1440", 2560, 1440, 30, 24, 4450.0}, {"2560x1440", 2560, 1440, 30, 25, 3543.0}, {"2560x1440", 2560, 1440, 30, 26, 2845.0}, {"2560x1440", 2560, 1440, 30, 27, 2311.0}, {"2560x1440", 2560, 1440, 30, 28, 1897.0}, {"2560x1440", 2560, 1440, 30, 29, 1579.0}, {"2560x1440", 2560, 1440, 30, 30, 1327.0}, {"2560x1440", 2560, 1440, 30, 31, 1121.0}, {"2560x1440", 2560, 1440, 30, 32, 964.0},
        // H.265 Data (Variable FPS for 480p) - Only CRF=32 remains
        {"854x480", 854, 480, 5, 32, 181}, {"854x480", 854, 480, 10, 32, 183}, {"854x480", 854, 480, 15, 32, 183}, {"854x480", 854, 480, 20, 32, 182}, {"854x480", 854, 480, 25, 32, 179},

        // VP9 Data (30 FPS)
        {"854x480", 854, 480, 30, 18, 1786.0}, {"854x480", 854, 480, 30, 19, 1600.0}, {"854x480", 854, 480, 30, 20, 1287.0}, {"854x480", 854, 480, 30, 21, 1079.0}, {"854x480", 854, 480, 30, 22, 918.0}, {"854x480", 854, 480, 30, 23, 760.0}, {"854x480", 854, 480, 30, 24, 636.0}, {"854x480", 854, 480, 30, 25, 528.0}, {"854x480", 854, 480, 30, 26, 438.0}, {"854x480", 854, 480, 30, 27, 368.0}, {"854x480", 854, 480, 30, 28, 310.0}, {"854x480", 854, 480, 30, 29, 285.0}, {"854x480", 854, 480, 30, 30, 263.0}, {"854x480", 854, 480, 30, 31, 240.0}, {"854x480", 854, 480, 30, 32, 222.0},
        {"1280x720", 1280, 720, 30, 18, 4129.0}, {"1280x720", 1280, 720, 30, 19, 3593.0}, {"1280x720", 1280, 720, 30, 20, 2840.0}, {"1280x720", 1280, 720, 30, 21, 2326.0}, {"1280x720", 1280, 720, 30, 22, 1920.0}, {"1280x720", 1280, 720, 30, 23, 1561.0}, {"1280x720", 1280, 720, 30, 24, 1287.0}, {"1280x720", 1280, 720, 30, 25, 1037.0}, {"1280x720", 1280, 720, 30, 26, 862.0}, {"1280x720", 1280, 720, 30, 27, 706.0}, {"1280x720", 1280, 720, 30, 28, 588.0}, {"1280x720", 1280, 720, 30, 29, 555.0}, {"1280x720", 1280, 720, 30, 30, 509.0}, {"1280x720", 1280, 720, 30, 31, 455.0}, {"1280x720", 1280, 720, 30, 32, 433.0},
        {"1920x1080", 1920, 1080, 30, 18, 10140.0}, {"1920x1080", 1920, 1080, 30, 19, 8785.0}, {"1920x1080", 1920, 1080, 30, 20, 6742.0}, {"1920x1080", 1920, 1080, 30, 21, 5344.0}, {"1920x1080", 1920, 1080, 30, 22, 4362.0}, {"1920x1080", 1920, 1080, 30, 23, 3370.0}, {"1920x1080", 1920, 1080, 30, 24, 2714.0}, {"1920x1080", 1920, 1080, 30, 25, 2120.0}, {"1920x1080", 1920, 1080, 30, 26, 1694.0}, {"1920x1080", 1920, 1080, 30, 27, 1393.0}, {"1920x1080", 1920, 1080, 30, 28, 1134.0}, {"1920x1080", 1920, 1080, 30, 29, 1050.0}, {"1920x1080", 1920, 1080, 30, 30, 952.0}, {"1920x1080", 1920, 1080, 30, 31, 876.0}, {"1920x1080", 1920, 1080, 30, 32, 793.0},
        {"2560x1440", 2560, 1440, 30, 18, 18862.0}, {"2560x1440", 2560, 1440, 30, 19, 16325.0}, {"2560x1440", 2560, 1440, 30, 20, 12665.0}, {"2560x1440", 2560, 1440, 30, 21, 10191.0}, {"2560x1440", 2560, 1440, 30, 22, 8279.0}, {"2560x1440", 2560, 1440, 30, 23, 6370.0}, {"2560x1440", 2560, 1440, 30, 24, 4991.0}, {"2560x1440", 2560, 1440, 30, 25, 3842.0}, {"2560x1440", 2560, 1440, 30, 26, 3012.0}, {"2560x1440", 2560, 1440, 30, 27, 2363.0}, {"2560x1440", 2560, 1440, 30, 28, 1880.0}, {"2560x1440", 2560, 1440, 30, 29, 1729.0}, {"2560x1440", 2560, 1440, 30, 30, 1536.0}, {"2560x1440", 2560, 1440, 30, 31, 1387.0}, {"2560x1440", 2560, 1440, 30, 32, 1269.0},
        // VP9 Data (Variable FPS for 480p) - Only CRF=32 remains
        {"854x480", 854, 480, 5, 32, 110}, {"854x480", 854, 480, 10, 32, 138}, {"854x480", 854, 480, 15, 32, 158}, {"854x480", 854, 480, 20, 32, 192}, {"854x480", 854, 480, 25, 32, 202},

        // AV1 Data (30 FPS)
        {"854x480", 854, 480, 30, 18, 966.0}, {"854x480", 854, 480, 30, 19, 874.0}, {"854x480", 854, 480, 30, 20, 748.0}, {"854x480", 854, 480, 30, 21, 656.0}, {"854x480", 854, 480, 30, 22, 585.0}, {"854x480", 854, 480, 30, 23, 510.0}, {"854x480", 854, 480, 30, 24, 454.0}, {"854x480", 854, 480, 30, 25, 404.0}, {"854x480", 854, 480, 30, 26, 359.0}, {"854x480", 854, 480, 30, 27, 323.0}, {"854x480", 854, 480, 30, 28, 288.0}, {"854x480", 854, 480, 30, 29, 274.0}, {"854x480", 854, 480, 30, 30, 260.0}, {"854x480", 854, 480, 30, 31, 247.0}, {"854x480", 854, 480, 30, 32, 235.0},
        {"1280x720", 1280, 720, 30, 18, 1928.0}, {"1280x720", 1280, 720, 30, 19, 1709.0}, {"1280x720", 1280, 720, 30, 20, 1430.0}, {"1280x720", 1280, 720, 30, 21, 1239.0}, {"1280x720", 1280, 720, 30, 22, 1092.0}, {"1280x720", 1280, 720, 30, 23, 945.0}, {"1280x720", 1280, 720, 30, 24, 835.0}, {"1280x720", 1280, 720, 30, 25, 736.0}, {"1280x720", 1280, 720, 30, 26, 662.0}, {"1280x720", 1280, 720, 30, 27, 588.0}, {"1280x720", 1280, 720, 30, 28, 526.0}, {"1280x720", 1280, 720, 30, 29, 503.0}, {"1280x720", 1280, 720, 30, 30, 476.0}, {"1280x720", 1280, 720, 30, 31, 447.0}, {"1280x720", 1280, 720, 30, 32, 427.0},
        {"1920x1080", 1920, 1080, 30, 18, 4323.0}, {"1920x1080", 1920, 1080, 30, 19, 3702.0}, {"1920x1080", 1920, 1080, 30, 20, 2846.0}, {"1920x1080", 1920, 1080, 30, 21, 2357.0}, {"1920x1080", 1920, 1080, 30, 22, 2010.0}, {"1920x1080", 1920, 1080, 30, 23, 1681.0}, {"1920x1080", 1920, 1080, 30, 24, 1456.0}, {"1920x1080", 1920, 1080, 30, 25, 1258.0}, {"1920x1080", 1920, 1080, 30, 26, 1105.0}, {"1920x1080", 1920, 1080, 30, 27, 978.0}, {"1920x1080", 1920, 1080, 30, 28, 871.0}, {"1920x1080", 1920, 1080, 30, 29, 820.0}, {"1920x1080", 1920, 1080, 30, 30, 775.0}, {"1920x1080", 1920, 1080, 30, 31, 738.0}, {"1920x1080", 1920, 1080, 30, 32, 695.0},
        {"2560x1440", 2560, 1440, 30, 18, 8462.0}, {"2560x1440", 2560, 1440, 30, 19, 7149.0}, {"2560x1440", 2560, 1440, 30, 20, 5428.0}, {"2560x1440", 2560, 1440, 30, 21, 4350.0}, {"2560x1440", 2560, 1440, 30, 22, 3564.0}, {"2560x1440", 2560, 1440, 30, 23, 2845.0}, {"2560x1440", 2560, 1440, 30, 24, 2364.0}, {"2560x1440", 2560, 1440, 30, 25, 1968.0}, {"2560x1440", 2560, 1440, 30, 26, 1683.0}, {"2560x1440", 2560, 1440, 30, 27, 1461.0}, {"2560x1440", 2560, 1440, 30, 28, 1270.0}, {"2560x1440", 2560, 1440, 30, 29, 1193.0}, {"2560x1440", 2560, 1440, 30, 30, 1120.0}, {"2560x1440", 2560, 1440, 30, 31, 1056.0}, {"2560x1440", 2560, 1440, 30, 32, 995.0},
        // AV1 Data (Variable FPS for 480p) - Only CRF=32 remains
        {"854x480", 854, 480, 5, 32, 108}, {"854x480", 854, 480, 10, 32, 152}, {"854x480", 854, 480, 15, 32, 182}, {"854x480", 854, 480, 20, 32, 208}, {"854x480", 854, 480, 25, 32, 227}
    
    };

    // 根据 m_codecType 筛选数据并填充 m_codecDb
    // 先清空，防止构造函数被意外多次调用
    m_codecDb.clear();
    
    // 定义各种编码器数据的边界索引
    const size_t h264_data_end_idx = 65; // H.264: 60 (30fps) + 5 (VFR) = 65 条
    const size_t h265_data_end_idx = 130; // H.265: 65 (H.264) + 65 (H.265) = 130 条
    const size_t vp9_data_end_idx = 195; // H.265: 65 (H.264) + 65 (H.265) + 65 (vp9) = 195 条

    
    if (m_codecType == "H.264") {
        m_codecDb.insert(m_codecDb.end(), fullDb.begin(), fullDb.begin() + h264_data_end_idx); 
    } else if (m_codecType == "H.265") {
        m_codecDb.insert(m_codecDb.end(), fullDb.begin() + h264_data_end_idx, fullDb.begin() + h265_data_end_idx);
    } else if (m_codecType == "VP9") {
        m_codecDb.insert(m_codecDb.end(), fullDb.begin() + h265_data_end_idx, fullDb.begin() + vp9_data_end_idx);
    } else { // AV1
        m_codecDb.insert(m_codecDb.end(), fullDb.begin() + vp9_data_end_idx, fullDb.end());
    }

    // 重新构建分辨率和帧率列表
    m_resolutions.clear();
    m_frameRates.clear();
    std::map<std::pair<int, int>, bool> seenResolutions; 
    std::map<int, bool> seenFrameRates;

    for (const auto& entry : m_codecDb) {
        // 收集分辨率
        std::pair<int, int> res = {entry.width, entry.height};
        if (seenResolutions.find(res) == seenResolutions.end()) {
            m_resolutions.push_back(res);
            seenResolutions[res] = true;
        }
        // 收集帧率
        if (seenFrameRates.find(entry.frameRate) == seenFrameRates.end()) {
            m_frameRates.push_back(entry.frameRate);
            seenFrameRates[entry.frameRate] = true;
        }
    }

    // 分辨率按面积升序排
    std::sort(m_resolutions.begin(), m_resolutions.end(), 
        [](const std::pair<int,int>& a, const std::pair<int,int>& b) {
        return (a.first * a.second) < (b.first * b.second);
    });
    
    // 帧率按升序排
    std::sort(m_frameRates.begin(), m_frameRates.end());


}


YtyCodecSimulator::~YtyCodecSimulator() {}


// +++获取分辨率在 m_resolutions 向量中的索引
int YtyCodecSimulator::GetResolutionIndex(const std::string& res_str) {
    for (size_t i = 0; i < m_resolutions.size(); ++i) {
        std::string current_res_str = std::to_string(m_resolutions[i].first) + "x" + std::to_string(m_resolutions[i].second);
        if (current_res_str == res_str) {
            return i;
        }
    }
    return -1; // Not found
}

// 获取帧率在 m_frameRates 向量中的索引
int YtyCodecSimulator::GetFrameRateIndex(int fps) {
    for (size_t i = 0; i < m_frameRates.size(); ++i) {
        if (m_frameRates[i] == fps) {
            return i;
        }
    }
    return -1; // Not found
}

// 请将 FindBestParams 函数的整个函数体替换为以下代码
EncodingParams YtyCodecSimulator::FindBestParams(double target_bitrate_kbps,
                                                 const std::string& current_res,
                                                 int current_fps,
                                                 int switch_res_direction)
{
    EncodingParams best_params;
    best_params.found = false;

    // 1. 确定要搜索的目标分辨率 (此部分逻辑与原来保持一致)
    int target_res_idx = GetResolutionIndex(current_res);
    if (target_res_idx == -1) { // 如果当前分辨率无效(比如是初始状态"N/A")，就从最低的开始
        target_res_idx = 0;
    }
    
    // 根据外部压力信号，尝试提升/降低目标分辨率
    if (switch_res_direction == 1) { 
        target_res_idx = std::min((int)m_resolutions.size() - 1, target_res_idx + 1);
    } else if (switch_res_direction == -1) {
        target_res_idx = std::max(0, target_res_idx - 1);
    }
    
    // --- [全新逻辑阶段一：在目标分辨率内进行两轮查找] ---
    // 我们的首要目标是在不改变分辨率的情况下找到最佳参数。
    {
        auto target_res = m_resolutions[target_res_idx];
        const CodecDataEntry* best_match_for_this_state = nullptr;
        double best_bitrate_so_far = -1.0;

        // --- 第一轮：稳定优先搜索 ---
        // 从摄像头当前的帧率开始向下找。这符合你的“压力检测”机制，尽量保持稳定。
        int start_fps_idx = GetFrameRateIndex(current_fps);
        if (start_fps_idx == -1) start_fps_idx = m_frameRates.size() - 1; // 安全检查

        for (int fps_idx = start_fps_idx; fps_idx >= 0; --fps_idx)
        {
            // (关键约束不变: 只有最低分辨率才允许大幅降低帧率)
            if (target_res_idx > 0 && fps_idx < (int)m_frameRates.size() - 1) {
                continue; 
            }
            int target_fps = m_frameRates[fps_idx];
            
            // 在给定的 分辨率-帧率 组合下查找最佳CRF
            for (const auto& entry : m_codecDb) {
                if (entry.width == target_res.first && entry.height == target_res.second && entry.frameRate == target_fps) {
                    if (entry.avgBitrate_kbps <= target_bitrate_kbps) {
                        if (entry.avgBitrate_kbps > best_bitrate_so_far) {
                            best_bitrate_so_far = entry.avgBitrate_kbps;
                            best_match_for_this_state = &entry;
                        }
                    }
                }
            }
        }

        // 如果在第一轮（稳定优先）就找到了，或者在第一轮之后进行一次全量搜索找到了，就直接返回
        if (best_match_for_this_state) {
            best_params.found = true;
            best_params.resolution = best_match_for_this_state->resolution;
            best_params.frame_rate = best_match_for_this_state->frameRate;
            best_params.crf = best_match_for_this_state->crf;
            best_params.actual_bitrate_kbps = best_match_for_this_state->avgBitrate_kbps;

            if (best_params.crf <= 20) best_params.qualityLevel = CRF_QUALITY_TOO_HIGH;
            else if (best_params.crf <= 28) best_params.qualityLevel = CRF_QUALITY_GOOD;
            else best_params.qualityLevel = CRF_QUALITY_TOO_LOW;
            
            return best_params;
        }
    }

    // --- [全新逻辑阶段二：紧急降级搜索] ---
    // 只有在阶段一完全失败（即目标分辨率在任何帧率下都找不到参数）后，才会进入此阶段。
    // 这就是你想要的“实在找不到码率了才能快速降档”。
    for (int res_idx = target_res_idx - 1; res_idx >= 0; --res_idx)
    {
        auto target_res = m_resolutions[res_idx];
        const CodecDataEntry* best_match_for_this_state = nullptr;
        double best_bitrate_so_far = -1.0;
        
        // 在紧急降级时，我们总是从最高帧率开始进行最全面的搜索。
        int start_fps_idx = m_frameRates.size() - 1;

        for (int fps_idx = start_fps_idx; fps_idx >= 0; --fps_idx)
        {
            if (res_idx > 0 && fps_idx < (int)m_frameRates.size() - 1) {
                continue;
            }
            int target_fps = m_frameRates[fps_idx];
            
            for (const auto& entry : m_codecDb) {
                if (entry.width == target_res.first && entry.height == target_res.second && entry.frameRate == target_fps) {
                    if (entry.avgBitrate_kbps <= target_bitrate_kbps) {
                        if (entry.avgBitrate_kbps > best_bitrate_so_far) {
                            best_bitrate_so_far = entry.avgBitrate_kbps;
                            best_match_for_this_state = &entry;
                        }
                    }
                }
            }
        }

        if (best_match_for_this_state) {
            best_params.found = true;
            best_params.resolution = best_match_for_this_state->resolution;
            best_params.frame_rate = best_match_for_this_state->frameRate;
            best_params.crf = best_match_for_this_state->crf;
            best_params.actual_bitrate_kbps = best_match_for_this_state->avgBitrate_kbps;

            if (best_params.crf <= 20) best_params.qualityLevel = CRF_QUALITY_TOO_HIGH;
            else if (best_params.crf <= 28) best_params.qualityLevel = CRF_QUALITY_GOOD;
            else best_params.qualityLevel = CRF_QUALITY_TOO_LOW;
            
            return best_params;
        }
    }

    // --- [最终备用逻辑] ---
    // 如果经历了所有降级手段还是找不到，返回整个数据库中的绝对最低码率配置。
    const CodecDataEntry* absolute_lowest_config = nullptr;
    double min_bitrate_global = 1e9;
     for (const auto& entry : m_codecDb) {
        if (entry.avgBitrate_kbps < min_bitrate_global) {
            min_bitrate_global = entry.avgBitrate_kbps;
            absolute_lowest_config = &entry;
        }
    }
     if (absolute_lowest_config) {
        best_params.found = true;
        best_params.resolution = absolute_lowest_config->resolution;
        best_params.frame_rate = absolute_lowest_config->frameRate;
        best_params.crf = absolute_lowest_config->crf;
        best_params.actual_bitrate_kbps = absolute_lowest_config->avgBitrate_kbps;
        best_params.qualityLevel = CRF_QUALITY_TOO_LOW;
        return best_params;
    }

    return best_params;
}


} // namespace ns3