// 文件作用：把 iOS 当前系统代理设置转换为 libcurl 的连接参数。
// 安全边界：无法同步解析 PAC 或代理配置异常时返回失败，调用方不得偷偷改为直连。
#pragma once

#include <cstdio>
#include <cstring>

#include <CFNetwork/CFNetwork.h>
#include <curl/curl.h>

namespace maiChatNetwork {

struct SystemProxy {
    bool direct = true;
    long type = CURLPROXY_HTTP;
    char host[260] = {};
    long port = 0;
};

// 仅本机调试服务可直接连接；正式 HTTPS 根据当前网络的系统代理决定路由。
inline bool selectSystemProxy(const char* url, SystemProxy* route) {
    if (route == nullptr || url == nullptr) return false;
    if (std::strncmp(url, "http://127.0.0.1:", 17) == 0) return true;
    // CFNetwork 依据当前 Wi-Fi/蜂窝网络返回路由；此处只接受可同步使用的明确配置。
    CFDictionaryRef settings = CFNetworkCopySystemProxySettings();
    if (settings == nullptr) return false;
    CFURLRef target = CFURLCreateWithBytes(kCFAllocatorDefault, reinterpret_cast<const UInt8*>(url),
                                           std::strlen(url), kCFStringEncodingUTF8, nullptr);
    CFArrayRef candidates = target == nullptr ? nullptr : CFNetworkCopyProxiesForURL(target, settings);
    CFRelease(settings);
    if (target != nullptr) CFRelease(target);
    if (candidates == nullptr) return false;
    const bool hasCandidate = CFArrayGetCount(candidates) > 0;
    const auto* selected = hasCandidate ? static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(candidates, 0)) : nullptr;
    const auto type = selected == nullptr || CFGetTypeID(selected) != CFDictionaryGetTypeID()
                          ? nullptr : static_cast<CFStringRef>(CFDictionaryGetValue(selected, kCFProxyTypeKey));
    if (type == nullptr || CFGetTypeID(type) != CFStringGetTypeID()) {
        CFRelease(candidates);
        return false;
    }
    if (CFEqual(type, kCFProxyTypeNone)) {
        CFRelease(candidates);
        return true;
    }
    // CFNetwork 的 HTTPS 代理类型表示 CONNECT 隧道；SOCKS5 的主机名交给代理解析。
    const bool httpTunnel = CFEqual(type, kCFProxyTypeHTTP) || CFEqual(type, kCFProxyTypeHTTPS);
    const bool socks = CFEqual(type, kCFProxyTypeSOCKS);
    const auto host = static_cast<CFStringRef>(CFDictionaryGetValue(selected, kCFProxyHostNameKey));
    const auto port = static_cast<CFNumberRef>(CFDictionaryGetValue(selected, kCFProxyPortNumberKey));
    char rawHost[256] = {};
    int portNumber = 0;
    const bool valid = (httpTunnel || socks) && host != nullptr && port != nullptr &&
                       CFGetTypeID(host) == CFStringGetTypeID() && CFGetTypeID(port) == CFNumberGetTypeID() &&
                       CFStringGetCString(host, rawHost, sizeof(rawHost), kCFStringEncodingUTF8) &&
                       CFNumberGetValue(port, kCFNumberIntType, &portNumber) && portNumber > 0 && portNumber <= 65535 &&
                       rawHost[0] != '\0';
    if (valid) {
        // IPv6 代理主机加方括号；失败时不返回半初始化的代理给 curl。
        route->direct = false;
        route->type = socks ? CURLPROXY_SOCKS5_HOSTNAME : CURLPROXY_HTTP;
        route->port = portNumber;
        const int written = std::snprintf(route->host, sizeof(route->host),
                                          std::strchr(rawHost, ':') == nullptr ? "%s" : "[%s]", rawHost);
        if (written < 0 || static_cast<size_t>(written) >= sizeof(route->host)) {
            CFRelease(candidates);
            return false;
        }
    }
    CFRelease(candidates);
    return valid;
}

inline bool configureSystemProxy(CURL* curl, const SystemProxy& proxy) {
    // 显式覆盖进程环境中的代理变量，防止它与 iOS 当前系统代理不一致。
    if (proxy.direct) {
        return curl_easy_setopt(curl, CURLOPT_PROXY, "") == CURLE_OK &&
               curl_easy_setopt(curl, CURLOPT_NOPROXY, "*") == CURLE_OK;
    }
    return curl_easy_setopt(curl, CURLOPT_PROXY, proxy.host) == CURLE_OK &&
           curl_easy_setopt(curl, CURLOPT_PROXYPORT, proxy.port) == CURLE_OK &&
           curl_easy_setopt(curl, CURLOPT_PROXYTYPE, proxy.type) == CURLE_OK &&
           curl_easy_setopt(curl, CURLOPT_NOPROXY, "") == CURLE_OK &&
           (proxy.type != CURLPROXY_HTTP ||
            curl_easy_setopt(curl, CURLOPT_HTTPPROXYTUNNEL, 1L) == CURLE_OK);
}

}  // namespace maiChatNetwork
