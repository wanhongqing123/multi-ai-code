#include "MaiNetworkIpTool.h"

#include <curl/curl.h>
#include <json.hpp>

#include <array>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#endif

namespace {

using Json = nlohmann::json;
std::once_flag sCurlInit;

std::size_t receiveBody(char* bytes, std::size_t size, std::size_t count, void* opaque) {
    auto& body = *static_cast<std::string*>(opaque);
    if (size > 0 && count > (1024 - body.size()) / size) return 0;
    body.append(bytes, size * count);
    return size * count;
}

bool validIp(const std::string& address) {
    std::array<unsigned char, 16> bytes{};
#if defined(_WIN32)
    return ::InetPtonA(AF_INET, address.c_str(), bytes.data()) == 1 ||
           ::InetPtonA(AF_INET6, address.c_str(), bytes.data()) == 1;
#else
    return ::inet_pton(AF_INET, address.c_str(), bytes.data()) == 1 ||
           ::inet_pton(AF_INET6, address.c_str(), bytes.data()) == 1;
#endif
}

class NetworkIpTool final : public MaiTool {
public:
    explicit NetworkIpTool(std::string caBundlePath) : mCaBundlePath(std::move(caBundlePath)) {}
    std::string name() const override {
        return "network_ip";
    }
    std::string description() const override {
        // 本机接口地址不需要联网；公网出口地址来自外部查询，可能是运营商 NAT 的地址。
        return "Get local IPv4/IPv6 interface addresses without network access, or query the "
               "public address observed by api64.ipify.org over HTTPS. Public IP may be a "
               "carrier NAT, VPN or proxy exit address, not an address assigned to the device.";
    }
    std::string parametersSchema() const override {
        // scope=local 查网卡地址，public 查网络出口，all 两者都查；默认为工具自己的安全值。
        return R"({"type":"object","properties":{"scope":{"type":"string","enum":["local","public","all"]}},"required":[],"additionalProperties":false})";
    }
    MaiToolResult execute(const std::string& raw, const MaiToolContext& context) override {
        const Json args = Json::parse(raw, nullptr, false);
        if (!args.is_object() || (args.contains("scope") && !args["scope"].is_string()))
            return MaiToolResult::failure(MaiErrorCode::InvalidInput, "invalid scope");
        const std::string scope = args.value("scope", std::string("local"));
        if (scope != "local" && scope != "public" && scope != "all")
            return MaiToolResult::failure(MaiErrorCode::InvalidInput, "invalid scope");
        Json response = Json::object();
        if (scope != "public") {
            Json addresses = Json::array();
            for (const auto& address : maiLocalIpAddresses())
                addresses.push_back(Json{{"interface", address.interfaceName},
                                         {"family", address.family},
                                         {"address", address.address}});
            response["local"] = std::move(addresses);
        }
        if (scope != "local") {
            if (context.isCanceled())
                return MaiToolResult::failure(MaiErrorCode::Canceled, "IP lookup was canceled");
            auto address = maiPublicIpAddress(mCaBundlePath);
            if (!address)
                return MaiToolResult::failure(address.error().code(), address.error().message());
            response["public"] =
                Json{{"address", address.value()},
                     {"family", address.value().find(':') == std::string::npos ? "IPv4" : "IPv6"},
                     {"source", "https://api64.ipify.org?format=json"}};
        }
        return MaiToolResult::success(response.dump());
    }

private:
    std::string mCaBundlePath;
};

}  // namespace

std::vector<MaiLocalIpAddress> maiLocalIpAddresses() {
    std::vector<MaiLocalIpAddress> result;
#if defined(_WIN32)
    ULONG length = 16384;
    std::vector<unsigned char> buffer(length);
    auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    ULONG status = ::GetAdaptersAddresses(
        AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
        nullptr, adapters, &length);
    if (status == ERROR_BUFFER_OVERFLOW && length <= 1024 * 1024) {
        buffer.resize(length);
        adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
        status = ::GetAdaptersAddresses(
            AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
            nullptr, adapters, &length);
    }
    if (status != NO_ERROR) return result;
    for (auto* adapter = adapters; adapter; adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
            continue;
        for (auto* item = adapter->FirstUnicastAddress; item; item = item->Next) {
            const int family = item->Address.lpSockaddr->sa_family;
            if (family != AF_INET && family != AF_INET6) continue;
            std::array<char, INET6_ADDRSTRLEN> text{};
            const void* bytes =
                family == AF_INET
                    ? static_cast<const void*>(
                          &reinterpret_cast<sockaddr_in*>(item->Address.lpSockaddr)->sin_addr)
                    : static_cast<const void*>(
                          &reinterpret_cast<sockaddr_in6*>(item->Address.lpSockaddr)->sin6_addr);
            if (::InetNtopA(family, const_cast<void*>(bytes), text.data(), text.size()))
                result.push_back(
                    {adapter->AdapterName, family == AF_INET ? "IPv4" : "IPv6", text.data()});
        }
    }
#else
    ifaddrs* list = nullptr;
    if (::getifaddrs(&list) != 0) return result;
    for (ifaddrs* item = list; item; item = item->ifa_next) {
        if (!item->ifa_addr || !(item->ifa_flags & IFF_UP) || (item->ifa_flags & IFF_LOOPBACK))
            continue;
        const int family = item->ifa_addr->sa_family;
        if (family != AF_INET && family != AF_INET6) continue;
        std::array<char, INET6_ADDRSTRLEN> text{};
        const void* bytes = family == AF_INET
                                ? static_cast<const void*>(
                                      &reinterpret_cast<sockaddr_in*>(item->ifa_addr)->sin_addr)
                                : static_cast<const void*>(
                                      &reinterpret_cast<sockaddr_in6*>(item->ifa_addr)->sin6_addr);
        if (::inet_ntop(family, bytes, text.data(), text.size()))
            result.push_back({item->ifa_name, family == AF_INET ? "IPv4" : "IPv6", text.data()});
    }
    ::freeifaddrs(list);
#endif
    return result;
}

MaiResult<std::string> maiPublicIpAddress(const std::string& caBundlePath) {
    std::call_once(sCurlInit, [] { ::curl_global_init(CURL_GLOBAL_DEFAULT); });
    CURL* curl = ::curl_easy_init();
    if (!curl) return {MaiErrorCode::Internal, "cannot initialize IP lookup"};
    std::string body;
    ::curl_easy_setopt(curl, CURLOPT_URL, "https://api64.ipify.org?format=json");
    ::curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receiveBody);
    ::curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    ::curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 5000L);
    ::curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    ::curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
    if (!caBundlePath.empty()) ::curl_easy_setopt(curl, CURLOPT_CAINFO, caBundlePath.c_str());
    const CURLcode code = ::curl_easy_perform(curl);
    long status = 0;
    ::curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    ::curl_easy_cleanup(curl);
    if (code != CURLE_OK || status != 200)
        return {MaiErrorCode::Network, "public IP lookup failed"};
    const Json response = Json::parse(body, nullptr, false);
    if (!response.is_object() || !response.value("ip", Json{}).is_string())
        return {MaiErrorCode::Protocol, "public IP service returned invalid data"};
    const std::string address = response["ip"].get<std::string>();
    if (!validIp(address)) return {MaiErrorCode::Protocol, "public IP service returned invalid IP"};
    return address;
}

std::unique_ptr<MaiTool> makeMaiNetworkIpTool(std::string caBundlePath) {
    return std::make_unique<NetworkIpTool>(std::move(caBundlePath));
}
