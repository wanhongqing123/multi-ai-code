#pragma once

#ifdef Q_OS_WIN
#include "ITXLiteAVNetworkProxy.h"
#else

// 当前 macOS TRTC framework 导出了这组公开接口，但 SDK 包漏带了对应的
// C++ 头文件。声明与同版本 Windows SDK 的 ITXLiteAVNetworkProxy.h 保持一致。
struct TRTCSocks5ProxyConfig {
    bool support_https = true;
    bool support_tcp = true;
    bool support_udp = true;
};

class ITXNetworkProxy {
protected:
    virtual ~ITXNetworkProxy() = default;

public:
    virtual int setSocks5Proxy(const char* host, unsigned short port,
                               const char* username, const char* password,
                               const TRTCSocks5ProxyConfig* config = nullptr) = 0;
};

extern "C" ITXNetworkProxy* createTXNetworkProxy();
extern "C" void destroyTXNetworkProxy(ITXNetworkProxy** proxy);

#endif
