/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */

/*
新增 BitrateSampler 码率采样器类:
我创建了两个全新的文件：model/yty-bitrate-sampler.h 和 model/yty-bitrate-sampler.cc。
这个类是本次修改的核心，它专门负责根据您设定的多峰正态分布参数（均值、标准差、权重）来生成一个符合统计规律的码率值。
它内部封装了复杂的采样逻辑，包括选择哪个分布、从该分布中采样、以及处理边界情况（如采样值低于300bps时取300bps）。
*/
#ifndef YTY_BITRATE_SAMPLER_H
#define YTY_BITRATE_SAMPLER_H

#include "ns3/object.h"
#include "ns3/nstime.h"
#include "ns3/random-variable-stream.h"
#include <vector>

namespace ns3 {

/**
 * @brief 从多峰正态分布中采样码率的类
 */
class BitrateSampler : public Object
{
public:
    static TypeId GetTypeId(void);
    BitrateSampler();
    virtual ~BitrateSampler();

    // 单个正态分布的参数结构体
    struct NormalDistribution {
        double mean;
        double stddev;
        double weight;
    };

    /**
     * @brief 添加一个正态分布模型到采样器中
     * @param mean 分布的均值 (bps)
     * @param stddev 分布的标准差 (bps)
     * @param weight 分布的权重 (0.0 to 1.0)
     */
    void AddDistribution(double mean, double stddev, double weight);

    /**
     * @brief 从配置的多峰分布中采样一个码率值
     * @return 采样到的码率值 (bps)
     */
    uint32_t Sample();

private:
    std::vector<NormalDistribution> m_distributions; // 存储所有分布模型
    Ptr<UniformRandomVariable> m_uniformSampler;     // 用于根据权重选择分布
    double m_totalWeight;                            // 总权重，用于归一化
};

} // namespace ns3

#endif /* YTY_BITRATE_SAMPLER_H */