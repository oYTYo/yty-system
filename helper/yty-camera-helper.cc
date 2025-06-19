/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */

#include "yty-camera-helper.h"
#include "yty-camera.h"
#include "ns3/uinteger.h"
#include "ns3/string.h"
#include "ns3/names.h"

namespace ns3 {

// 构造函数，初始化对象工厂并设置远程地址和端口属性。
YtyCameraHelper::YtyCameraHelper(Address ip, uint16_t port)
{
    m_factory.SetTypeId(YtyCamera::GetTypeId());
    SetAttribute("RemoteAddress", AddressValue(ip));
    SetAttribute("RemotePort", UintegerValue(port));
}

// 设置应用属性的方法。
void YtyCameraHelper::SetAttribute(std::string name, const AttributeValue &value)
{
    m_factory.Set(name, value);
}

// 在节点容器中的所有节点上安装应用的公共方法。
ApplicationContainer YtyCameraHelper::Install(NodeContainer c) const
{
    ApplicationContainer apps;
    for (auto i = c.Begin(); i != c.End(); ++i)
    {
        apps.Add(InstallPriv(*i));
    }
    return apps;
}

// 在单个节点上安装应用的公共方法。
ApplicationContainer YtyCameraHelper::Install(Ptr<Node> node) const
{
    return ApplicationContainer(InstallPriv(node));
}

// 私有的安装实现，创建应用并将其添加到节点。
Ptr<Application> YtyCameraHelper::InstallPriv(Ptr<Node> node) const
{
    Ptr<Application> app = m_factory.Create<YtyCamera>();
    node->AddApplication(app);
    return app;
}

} // namespace ns3