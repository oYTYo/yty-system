/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */

#ifndef YTY_CODEC_SIMULATOR_H
#define YTY_CODEC_SIMULATOR_H

#include "ns3/object.h"
#include <string>
#include <vector>

namespace ns3 {

/**
 * @brief 编码器参数结构体，用于返回查找结果
 */
struct EncodingParams {
    std::string resolution;
    int         crf;
    int         frame_rate;
    double      actual_bitrate_kbps;
    bool        found; // 标记是否成功找到配置

    EncodingParams() : crf(0), frame_rate(0), actual_bitrate_kbps(0.0), found(false) {}
};

/**
 * @brief 模拟真实编码器的参数选择逻辑
 *
 * 这个类根据给定的目标码率，从一个预定义的编码参数数据库中
 * 查找最佳的分辨率、帧率和CRF值。
 * 它的逻辑复现自 real_codec.py 脚本。
 */
class YtyCodecSimulator : public Object
{
public:
    static TypeId GetTypeId(void);
    YtyCodecSimulator();
    // 构造函数，需要指定使用 H.264 还是 H.265
    YtyCodecSimulator(std::string codecType);
    virtual ~YtyCodecSimulator();

    /**
     * @brief 根据目标码率查找最佳编码参数
     * @param target_bitrate_kbps 服务器下发的目标可用带宽 (单位: kbps)
     * @return 包含最佳参数的 EncodingParams 结构体
     */
    EncodingParams FindParams(double target_bitrate_kbps);

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
    std::vector<CodecDataEntry> m_codecDb; // 存储编码参数的数据库
    std::vector<int> m_frameRates;         // 支持的帧率列表 (降序)
    std::vector<std::pair<int, int>> m_resolutions; // 支持的分辨率列表 (降序)
};

} // namespace ns3

#endif /* YTY_CODEC_SIMULATOR_H */