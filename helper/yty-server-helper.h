/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */

#ifndef YTY_SERVER_HELPER_H
#define YTY_SERVER_HELPER_H

#include "ns3/application-container.h"
#include "ns3/node-container.h"
#include "ns3/object-factory.h"
#include <stdint.h>

namespace ns3 {

/**
 * @brief 一个用于创建 YtyServer 应用的辅助类
 */
class YtyServerHelper
{
public:
    /**
     * @brief 构造函数，需要指定服务器监听的端口。
     * @param port 服务器监听的端口号。
     */
    YtyServerHelper(uint16_t port);

    /**
     * @brief 设置将在每个创建的应用上应用的属性。
     * @param name 要设置的属性名称。
     * @param value 要设置的属性值。
     */
    void SetAttribute(std::string name, const AttributeValue &value);

    /**
     * @brief 在指定的节点上安装 YtyServer 应用。
     * @param c 要安装应用的节点容器。
     * @return 返回一个包含所有已安装应用的 ApplicationContainer。
     */
    ApplicationContainer Install(NodeContainer c) const;
    
    /**
     * @brief 在指定的单个节点上安装 YtyServer 应用。
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

#endif /* YTY_SERVER_HELPER_H */