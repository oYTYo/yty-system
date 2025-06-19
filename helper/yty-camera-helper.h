/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */

#ifndef YTY_CAMERA_HELPER_H
#define YTY_CAMERA_HELPER_H

#include "ns3/application-container.h"
#include "ns3/node-container.h"
#include "ns3/object-factory.h"
#include "ns3/ipv4-address.h"

namespace ns3 {

/**
 * @brief 一个用于创建 YtyCamera 应用的辅助类
 */
class YtyCameraHelper
{
public:
    /**
     * @brief 构造函数，需要指定远程服务器的地址和端口。
     * @param ip 远程服务器的IP地址。
     * @param port 远程服务器的端口号。
     */
    YtyCameraHelper(Address ip, uint16_t port);

    /**
     * @brief 设置将在每个创建的应用上应用的属性。
     * @param name 要设置的属性名称。
     * @param value 要设置的属性值。
     */
    void SetAttribute(std::string name, const AttributeValue &value);

    /**
     * @brief 在指定的节点上安装 YtyCamera 应用。
     * @param c 要安装应用的节点容器。
     * @return 返回一个包含所有已安装应用的 ApplicationContainer。
     */
    ApplicationContainer Install(NodeContainer c) const;

    /**
     * @brief 在指定的单个节点上安装 YtyCamera 应用。
     * @param node 指向要安装应用的节点的指针。
     * @return 返回一个包含已安装应用的 ApplicationContainer。
     */
    ApplicationContainer Install(Ptr<Node> node) const;

private:
    /**
     * @brief 私有的安装函数，实际执行应用的创建和安装。
     * @param node 指向要安装应用的节点的指针。
     * @return 返回一个指向已创建应用的智能指针。
     */
    Ptr<Application> InstallPriv(Ptr<Node> node) const;

    ObjectFactory m_factory; // 用于创建应用实例的对象工厂。
};

} // namespace ns3

#endif /* YTY_CAMERA_HELPER_H */