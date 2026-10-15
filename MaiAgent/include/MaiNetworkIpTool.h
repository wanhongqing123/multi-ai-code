// 文件作用：查询网络接口地址并向 Agent 返回可读结果。
// 对外类型：MaiLocalIpAddress；线程、失败和生命周期以类型附近注释为准。
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "MaiError.h"
#include "MaiTool.h"

struct MaiLocalIpAddress {
    std::string interfaceName;
    std::string family;
    std::string address;
};

// 通过操作系统接口列举活动且非回环的网卡地址，不发网络请求。
std::vector<MaiLocalIpAddress> maiLocalIpAddresses();

// 请求固定的 HTTPS 回显端点，取得外部看到的地址。
// 返回值可能属于 VPN、移动运营商 NAT 或已配置代理；需要网络连接。
MaiResult<std::string> maiPublicIpAddress(const std::string& caBundlePath = {});

std::unique_ptr<MaiTool> makeMaiNetworkIpTool(std::string caBundlePath = {});
