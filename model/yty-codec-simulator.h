/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */

#ifndef YTY_CODEC_SIMULATOR_H
#define YTY_CODEC_SIMULATOR_H

#include "ns3/object.h"
#include <string>
#include <vector>

namespace ns3 {


// 定义一个枚举来表示CRF所处的质量区间
enum CrfQualityLevel {
    CRF_QUALITY_TOO_HIGH, // CRF 18-20, 质量过好，可以考虑升档
    CRF_QUALITY_GOOD,     // CRF 21-28, 质量理想，保持稳定
    CRF_QUALITY_TOO_LOW   // CRF 29-32, 质量较差，可以考虑降档
};



/*@brief 编码器参数结构体，用于返回查找结果*/
struct EncodingParams {
    std::string resolution;
    int         crf;
    int         frame_rate;
    double      actual_bitrate_kbps;
    bool        found; // 标记是否成功找到配置

    // +++ [新增] +++ 返回当前参数组合的质量评估
    CrfQualityLevel qualityLevel;

    EncodingParams() : crf(0), frame_rate(0), actual_bitrate_kbps(0.0), found(false), qualityLevel(CRF_QUALITY_GOOD) {}
};



/*@brief 模拟真实编码器的参数选择逻辑*/

class YtyCodecSimulator : public Object
{

public:
    static TypeId GetTypeId(void);
    YtyCodecSimulator();
    // 构造函数，需要指定使用 H.264 还是 H.265
    YtyCodecSimulator(std::string codecType, std::string complexity = "normal");
    virtual ~YtyCodecSimulator();

    /**
     * @brief 根据目标码率和当前状态查找最佳编码参数
     * @param target_bitrate_kbps 服务器下发的目标可用带宽 (单位: kbps)
     * @param current_res 当前的分辨率
     * @param current_fps 当前的帧率
     * @param switch_res_direction 允许的分辨率切换方向 (-1: 降, 0: 不切换, 1: 升)
     * @return 包含最佳参数的 EncodingParams 结构体
     */
    EncodingParams FindBestParams(double target_bitrate_kbps, 
                                  const std::string& current_res, 
                                  int current_fps,
                                  int switch_res_direction);


private:
    // 用于存储从CSV中提取的数据
    struct CodecDataEntry {
        std::string resolution;
        int         width;
        int         height;
        int         frameRate;
        int         crf;
        double      avgBitrate_kbps;
    };
    
    std::string m_codecType;
    std::string m_complexity;
    std::vector<CodecDataEntry> m_codecDb; // 存储编码参数的数据库
    std::vector<std::pair<int, int>> m_resolutions; // 支持的分辨率列表 (降序)

    std::vector<int> m_frameRates;  // 支持的帧率列表 (降序)

    // 用于获取分辨率的索引，方便查找上一档/下一档
    int GetResolutionIndex(const std::string& res_str);

    int GetFrameRateIndex(int fps);  // 用于获取帧率的索引，方便查找上一档/下一档


};

} // namespace ns3

#endif /* YTY_CODEC_SIMULATOR_H */