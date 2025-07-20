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
        // H.264 Data
        {"854x480", 854, 480, 30, 18, 1838.0}, {"854x480", 854, 480, 30, 19, 1553.0}, {"854x480", 854, 480, 30, 20, 1320.0}, {"854x480", 854, 480, 30, 21, 1124.0}, {"854x480", 854, 480, 30, 22, 962.0}, {"854x480", 854, 480, 30, 23, 829.0}, {"854x480", 854, 480, 30, 24, 719.0}, {"854x480", 854, 480, 30, 25, 627.0}, {"854x480", 854, 480, 30, 26, 550.0}, {"854x480", 854, 480, 30, 27, 484.0}, {"854x480", 854, 480, 30, 28, 428.0}, {"854x480", 854, 480, 30, 29, 380.0}, {"854x480", 854, 480, 30, 30, 338.0}, {"854x480", 854, 480, 30, 31, 302.0}, {"854x480", 854, 480, 30, 32, 271.0},
        {"1280x720", 1280, 720, 30, 18, 4315.0}, {"1280x720", 1280, 720, 30, 19, 3513.0}, {"1280x720", 1280, 720, 30, 20, 2890.0}, {"1280x720", 1280, 720, 30, 21, 2395.0}, {"1280x720", 1280, 720, 30, 22, 2010.0}, {"1280x720", 1280, 720, 30, 23, 1705.0}, {"1280x720", 1280, 720, 30, 24, 1457.0}, {"1280x720", 1280, 720, 30, 25, 1261.0}, {"1280x720", 1280, 720, 30, 26, 1103.0}, {"1280x720", 1280, 720, 30, 27, 967.0}, {"1280x720", 1280, 720, 30, 28, 851.0}, {"1280x720", 1280, 720, 30, 29, 755.0}, {"1280x720", 1280, 720, 30, 30, 672.0}, {"1280x720", 1280, 720, 30, 31, 602.0}, {"1280x720", 1280, 720, 30, 32, 542.0},
        {"1920x1080", 1920, 1080, 30, 18, 11831.0}, {"1920x1080", 1920, 1080, 30, 19, 9367.0}, {"1920x1080", 1920, 1080, 30, 20, 7428.0}, {"1920x1080", 1920, 1080, 30, 21, 5923.0}, {"1920x1080", 1920, 1080, 30, 22, 4769.0}, {"1920x1080", 1920, 1080, 30, 23, 3901.0}, {"1920x1080", 1920, 1080, 30, 24, 3250.0}, {"1920x1080", 1920, 1080, 30, 25, 2747.0}, {"1920x1080", 1920, 1080, 30, 26, 2360.0}, {"1920x1080", 1920, 1080, 30, 27, 2045.0}, {"1920x1080", 1920, 1080, 30, 28, 1788.0}, {"1920x1080", 1920, 1080, 30, 29, 1577.0}, {"1920x1080", 1920, 1080, 30, 30, 1400.0}, {"1920x1080", 1920, 1080, 30, 31, 1254.0}, {"1920x1080", 1920, 1080, 30, 32, 1128.0},
        {"2560x1440", 2560, 1440, 30, 18, 23612.0}, {"2560x1440", 2560, 1440, 30, 19, 18932.0}, {"2560x1440", 2560, 1440, 30, 20, 15097.0}, {"2560x1440", 2560, 1440, 30, 21, 11976.0}, {"2560x1440", 2560, 1440, 30, 22, 9483.0}, {"2560x1440", 2560, 1440, 30, 23, 7555.0}, {"2560x1440", 2560, 1440, 30, 24, 6100.0}, {"2560x1440", 2560, 1440, 30, 25, 5011.0}, {"2560x1440", 2560, 1440, 30, 26, 4188.0}, {"2560x1440", 2560, 1440, 30, 27, 3576.0}, {"2560x1440", 2560, 1440, 30, 28, 3087.0}, {"2560x1440", 2560, 1440, 30, 29, 2700.0}, {"2560x1440", 2560, 1440, 30, 30, 2385.0}, {"2560x1440", 2560, 1440, 30, 31, 2124.0}, {"2560x1440", 2560, 1440, 30, 32, 1902.0},
        // H.265 Data
        {"854x480", 854, 480, 30, 18, 1515.0}, {"854x480", 854, 480, 30, 19, 1285.0}, {"854x480", 854, 480, 30, 20, 1090.0}, {"854x480", 854, 480, 30, 21, 931.0}, {"854x480", 854, 480, 30, 22, 799.0}, {"854x480", 854, 480, 30, 23, 687.0}, {"854x480", 854, 480, 30, 24, 593.0}, {"854x480", 854, 480, 30, 25, 513.0}, {"854x480", 854, 480, 30, 26, 442.0}, {"854x480", 854, 480, 30, 27, 383.0}, {"854x480", 854, 480, 30, 28, 332.0}, {"854x480", 854, 480, 30, 29, 288.0}, {"854x480", 854, 480, 30, 30, 251.0}, {"854x480", 854, 480, 30, 31, 218.0}, {"854x480", 854, 480, 30, 32, 190.0},
        {"1280x720", 1280, 720, 30, 18, 3472.0}, {"1280x720", 1280, 720, 30, 19, 2853.0}, {"1280x720", 1280, 720, 30, 20, 2348.0}, {"1280x720", 1280, 720, 30, 21, 1951.0}, {"1280x720", 1280, 720, 30, 22, 1636.0}, {"1280x720", 1280, 720, 30, 23, 1378.0}, {"1280x720", 1280, 720, 30, 24, 1169.0}, {"1280x720", 1280, 720, 30, 25, 994.0}, {"1280x720", 1280, 720, 30, 26, 848.0}, {"1280x720", 1280, 720, 30, 27, 729.0}, {"1280x720", 1280, 720, 30, 28, 627.0}, {"1280x720", 1280, 720, 30, 29, 542.0}, {"1280x720", 1280, 720, 30, 30, 469.0}, {"1280x720", 1280, 720, 30, 31, 406.0}, {"1280x720", 1280, 720, 30, 32, 351.0},
        {"1920x1080", 1920, 1080, 30, 18, 8609.0}, {"1920x1080", 1920, 1080, 30, 19, 6917.0}, {"1920x1080", 1920, 1080, 30, 20, 5528.0}, {"1920x1080", 1920, 1080, 30, 21, 4422.0}, {"1920x1080", 1920, 1080, 30, 22, 3559.0}, {"1920x1080", 1920, 1080, 30, 23, 2879.0}, {"1920x1080", 1920, 1080, 30, 24, 2348.0}, {"1920x1080", 1920, 1080, 30, 25, 1942.0}, {"1920x1080", 1920, 1080, 30, 26, 1611.0}, {"1920x1080", 1920, 1080, 30, 27, 1353.0}, {"1920x1080", 1920, 1080, 30, 28, 1148.0}, {"1920x1080", 1920, 1080, 30, 29, 980.0}, {"1920x1080", 1920, 1080, 30, 30, 843.0}, {"1920x1080", 1920, 1080, 30, 31, 727.0}, {"1920x1080", 1920, 1080, 30, 32, 629.0},
        {"2560x1440", 2560, 1440, 30, 18, 17278.0}, {"2560x1440", 2560, 1440, 30, 19, 14000.0}, {"2560x1440", 2560, 1440, 30, 20, 11235.0}, {"2560x1440", 2560, 1440, 30, 21, 8946.0}, {"2560x1440", 2560, 1440, 30, 22, 7100.0}, {"2560x1440", 2560, 1440, 30, 23, 5613.0}, {"2560x1440", 2560, 1440, 30, 24, 4450.0}, {"2560x1440", 2560, 1440, 30, 25, 3543.0}, {"2560x1440", 2560, 1440, 30, 26, 2845.0}, {"2560x1440", 2560, 1440, 30, 27, 2311.0}, {"2560x1440", 2560, 1440, 30, 28, 1897.0}, {"2560x1440", 2560, 1440, 30, 29, 1579.0}, {"2560x1440", 2560, 1440, 30, 30, 1327.0}, {"2560x1440", 2560, 1440, 30, 31, 1121.0}, {"2560x1440", 2560, 1440, 30, 32, 964.0}
    };

    // 只需要 seenResolutions，因为帧率已固定为30，m_frameRates 不再需要动态构建和排序
    std::map<std::pair<int, int>, bool> seenResolutions; 

    if(m_codecType == "H.264") {
        m_codecDb.insert(m_codecDb.end(), fullDb.begin(), fullDb.begin() + 60); // 假设 H.264 数据有60条，请根据实际更新后的 fullDb 数量调整
    } else { // H.265
        m_codecDb.insert(m_codecDb.end(), fullDb.begin() + 60, fullDb.end()); // 假设 H.265 数据从第60条开始，请根据实际更新后的 fullDb 数量调整
    }

    for (const auto& entry : m_codecDb) {
        // 仅处理分辨率的收集，帧率不再需要单独列表
        std::pair<int, int> res = {entry.width, entry.height};
        if (seenResolutions.find(res) == seenResolutions.end()) {
            m_resolutions.push_back(res);
            seenResolutions[res] = true;
        }
    }

    // 分辨率按面积升序排
    std::sort(m_resolutions.begin(), m_resolutions.end(), 
        [](const std::pair<int,int>& a, const std::pair<int,int>& b) {
        return (a.first * a.second) < (b.first * b.second);
    });
}


YtyCodecSimulator::~YtyCodecSimulator() {}


// +++ [新增] +++ 获取分辨率在 m_resolutions 向量中的索引
int YtyCodecSimulator::GetResolutionIndex(const std::string& res_str) {
    for (size_t i = 0; i < m_resolutions.size(); ++i) {
        std::string current_res_str = std::to_string(m_resolutions[i].first) + "x" + std::to_string(m_resolutions[i].second);
        if (current_res_str == res_str) {
            return i;
        }
    }
    return -1; // Not found
}



EncodingParams YtyCodecSimulator::FindBestParams(double target_bitrate_kbps, 
                                                 const std::string& current_res, 
                                                 int current_fps,
                                                 int switch_res_direction)
{
    EncodingParams best_params;
    best_params.found = false;

    // 1. 确定要搜索的分辨率
    int target_res_idx = GetResolutionIndex(current_res);
    if (target_res_idx == -1) { // 如果当前分辨率无效，就从最低的开始
        target_res_idx = 0;
    }
    
    if (switch_res_direction == 1) { // 升档
        target_res_idx = std::min((int)m_resolutions.size() - 1, target_res_idx + 1);
    } else if (switch_res_direction == -1) { // 降档
        target_res_idx = std::max(0, target_res_idx - 1);
    }

    auto target_res = m_resolutions[target_res_idx];
    
    // 2. 在目标分辨率和当前帧率下查找最佳CRF
    const CodecDataEntry* best_match = nullptr;
    double best_bitrate_so_far = -1.0;

    for (const auto& entry : m_codecDb) {
        if (entry.width == target_res.first && entry.height == target_res.second && entry.frameRate == current_fps) {
            if (entry.avgBitrate_kbps <= target_bitrate_kbps) {
                // 在所有低于目标码率的选项里，找一个码率最高的(即CRF最低，画质最好)
                if (entry.avgBitrate_kbps > best_bitrate_so_far) {
                    best_bitrate_so_far = entry.avgBitrate_kbps;
                    best_match = &entry;
                }
            }
        }
    }

    // 3. 如果找到了匹配项
    if (best_match) {
        best_params.found = true;
        best_params.resolution = best_match->resolution;
        best_params.frame_rate = best_match->frameRate;
        best_params.crf = best_match->crf;
        best_params.actual_bitrate_kbps = best_match->avgBitrate_kbps;

        // 评估CRF质量
        if (best_params.crf <= 20) best_params.qualityLevel = CRF_QUALITY_TOO_HIGH;
        else if (best_params.crf <= 28) best_params.qualityLevel = CRF_QUALITY_GOOD;
        else best_params.qualityLevel = CRF_QUALITY_TOO_LOW;
        
        return best_params;
    }

    // 4. 如果在当前(或新)分辨率下没找到，说明目标码率太低了
    // 4.1. 检查是不是因为触发了升档，但新分辨率的最低码率都比目标高
    if (switch_res_direction == 1 && !best_match) {
        // 升档失败，回退到原来的分辨率再找一次
        return FindBestParams(target_bitrate_kbps, current_res, current_fps, 0);
    }

    // 4.2 降码率的逻辑删掉了

    // 5. 如果经历了所有降级手段还是找不到，就返回当前分辨率/帧率下的最低码率配置
    const CodecDataEntry* lowest_config_for_current_state = nullptr;
    double min_bitrate_in_state = 1e9;
    for (const auto& entry : m_codecDb) {
        if (entry.width == target_res.first && entry.height == target_res.second && entry.frameRate == current_fps) {
            if (entry.avgBitrate_kbps < min_bitrate_in_state) {
                min_bitrate_in_state = entry.avgBitrate_kbps;
                lowest_config_for_current_state = &entry;
            }
        }
    }

    if (lowest_config_for_current_state) {
        best_params.found = true;
        best_params.resolution = lowest_config_for_current_state->resolution;
        best_params.frame_rate = lowest_config_for_current_state->frameRate;
        best_params.crf = lowest_config_for_current_state->crf;
        best_params.actual_bitrate_kbps = lowest_config_for_current_state->avgBitrate_kbps;
        best_params.qualityLevel = CRF_QUALITY_TOO_LOW;
        return best_params;
    }

    // 最终备用方案：返回整个数据库中的最低配置
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


    return best_params; // 返回没找到
}


} // namespace ns3