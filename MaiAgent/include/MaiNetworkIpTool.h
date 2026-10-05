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
