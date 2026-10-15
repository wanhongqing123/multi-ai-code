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

// Enumerates active, non-loopback interface addresses with OS APIs; no network request.
std::vector<MaiLocalIpAddress> maiLocalIpAddresses();

// Queries a fixed HTTPS echo endpoint for the address observed outside this device. The returned
// address may belong to a VPN, mobile carrier NAT or configured proxy. Requires connectivity.
MaiResult<std::string> maiPublicIpAddress(const std::string& caBundlePath = {});

std::unique_ptr<MaiTool> makeMaiNetworkIpTool(std::string caBundlePath = {});
