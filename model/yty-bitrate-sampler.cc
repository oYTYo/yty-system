/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */

#include "yty-bitrate-sampler.h"
#include "ns3/log.h"
#include "ns3/double.h"
#include "ns3/uinteger.h"

namespace ns3 {

NS_LOG_COMPONENT_DEFINE("YtyBitrateSampler");
NS_OBJECT_ENSURE_REGISTERED(BitrateSampler);

TypeId BitrateSampler::GetTypeId(void)
{
    static TypeId tid = TypeId("ns3::BitrateSampler")
        .SetParent<Object>()
        .SetGroupName("Applications")
        .AddConstructor<BitrateSampler>();
    return tid;
}

BitrateSampler::BitrateSampler() : m_totalWeight(0.0)
{
    m_uniformSampler = CreateObject<UniformRandomVariable>();
}

BitrateSampler::~BitrateSampler()
{
}

void BitrateSampler::AddDistribution(double mean, double stddev, double weight)
{
    NS_LOG_FUNCTION(this << mean << stddev << weight);
    m_distributions.push_back({mean, stddev, weight});
    m_totalWeight += weight;
}

uint32_t BitrateSampler::Sample()
{
    NS_LOG_FUNCTION(this);
    if (m_distributions.empty())
    {
        NS_LOG_WARN("No distributions added to sampler. Returning 0.");
        return 0;
    }

    // 1. 根据权重选择一个分布
    double choice = m_uniformSampler->GetValue(0.0, m_totalWeight);
    double cumulativeWeight = 0.0;
    const NormalDistribution* selectedDist = nullptr;

    for (const auto& dist : m_distributions)
    {
        cumulativeWeight += dist.weight;
        if (choice <= cumulativeWeight)
        {
            selectedDist = &dist;
            break;
        }
    }
    
    // 如果由于浮点数精度问题没有选到，就选最后一个
    if (!selectedDist)
    {
        selectedDist = &m_distributions.back();
    }

    // 2. 从选定的正态分布中采样
    Ptr<NormalRandomVariable> normalSampler = CreateObject<NormalRandomVariable>();
    normalSampler->SetAttribute("Mean", DoubleValue(selectedDist->mean));
    normalSampler->SetAttribute("Variance", DoubleValue(selectedDist->stddev * selectedDist->stddev));
    
    double sampledValue = normalSampler->GetValue();

    // 3. 防报错逻辑：确保码率不小于300kbps并且不大于8M

    const uint32_t minBitrate = 300000;
    const uint32_t maxBitrate = 8000000;
    if (sampledValue < minBitrate)
    {
        return minBitrate;
    }
    if (sampledValue > maxBitrate)
    {
        return maxBitrate;
    }

    return static_cast<uint32_t>(sampledValue);
}

} // namespace ns3