/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */

#include "yty-server-helper.h"
#include "yty-server.h"
#include "ns3/uinteger.h"
#include "ns3/string.h"
#include "ns3/names.h"

namespace ns3 {

// 构造函数，初始化对象工厂并设置端口属性。
YtyServerHelper::YtyServerHelper(uint16_t port)
{
    m_factory.SetTypeId(YtyServer::GetTypeId());
    SetAttribute("Port", UintegerValue(port));
}

// 设置应用属性的方法。
void YtyServerHelper::SetAttribute(std::string name, const AttributeValue &value)
{
    m_factory.Set(name, value);
}

// 在节点容器中的所有节点上安装应用的公共方法。
ApplicationContainer YtyServerHelper::Install(NodeContainer c) const
{
    ApplicationContainer apps;
    for (auto i = c.Begin(); i != c.End(); ++i)
    {
        apps.Add(InstallPriv(*i));
    }
    return apps;
}

// 在单个节点上安装应用的公共方法。
ApplicationContainer YtyServerHelper::Install(Ptr<Node> node) const
{
    return ApplicationContainer(InstallPriv(node));
}

// 私有的安装实现，创建应用并将其添加到节点。
Ptr<Application> YtyServerHelper::InstallPriv(Ptr<Node> node) const
{
    Ptr<Application> app = m_factory.Create<YtyServer>();
    node->AddApplication(app);
    return app;
}

} // namespace ns3