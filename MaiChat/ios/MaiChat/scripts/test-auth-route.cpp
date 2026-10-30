// 定向验证 iOS 账号线路选择和写请求语义；替换网络结果，不访问真实账号或凭据。
#define MAICHAT_AUTH_ROUTE_TEST 1
#include "../MaiChat/IM/MaiChatAuthTransport.cpp"

#include <atomic>
#include <cassert>
#include <iostream>

static std::atomic_int systemCount{0}, aliCount{0}, cloudCount{0}, sends{0};
static std::atomic_bool systemWorks{false};

static bool fakeProbe(AuthRoute route) {
    if (route == AuthRoute::System) { ++systemCount; return systemWorks.load(); }
    if (route == AuthRoute::AliDoh) { ++aliCount; return true; }
    ++cloudCount;
    return false;
}

static CURLcode fakeBusiness() { ++sends; return CURLE_OPERATION_TIMEDOUT; }

int main(int argc, char** argv) {
    if (argc == 5 && std::string(argv[1]) == "--tls-bad-host") {
        assert(curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK);
        testDohUrl = argv[2];
        const std::string mapping = std::string("ichat.life:443:ichat.life:") + argv[3];
        testConnectTo = curl_slist_append(nullptr, mapping.c_str());
        maiChatNetwork::SystemProxy direct;
        assert(!probeRoute(AuthRoute::AliDoh, direct, argv[4], 5000));
        curl_slist_free_all(testConnectTo);
        std::cout << "iOS auth hostname mismatch rejected: PASS\n";
        return 0;
    }
    if (argc == 6 && std::string(argv[1]) == "--tls") {
        assert(curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK);
        testDohUrl = argv[2];
        const std::string mapping = std::string("ichat.life:443:ichat.life:") + argv[3];
        testConnectTo = curl_slist_append(nullptr, mapping.c_str());
        maiChatNetwork::SystemProxy direct;
        assert(!probeRoute(AuthRoute::AliDoh, direct, argv[5], 5000));
        assert(probeRoute(AuthRoute::AliDoh, direct, argv[4], 5000));
        assert(probeRoute(AuthRoute::CloudflareDoh, direct, argv[4], 5000));
        curl_slist_free_all(testConnectTo);
        std::cout << "iOS auth DoH, TLS and original hostname: PASS\n";
        return 0;
    }
    probeHook = fakeProbe;
    businessHook = fakeBusiness;
    maiChatNetwork::SystemProxy proxy;
    AuthRoute selected = AuthRoute::System;
    assert(chooseRoute(proxy, true, "/unused-test-ca.pem", &selected));
    assert(selected == AuthRoute::AliDoh && systemCount == 1 && aliCount >= 1);
    const int count = systemCount;
    assert(chooseRoute(proxy, true, "/unused-test-ca.pem", &selected));
    assert(systemCount == count);  // 同一系统配置在有效期内复用已验证线路。
    {
        std::lock_guard<std::mutex> lock(routeMutex);
        routeExpiry = {};
    }
    systemWorks = true;
    assert(chooseRoute(proxy, true, "/unused-test-ca.pem", &selected));
    assert(selected == AuthRoute::System);
    {
        std::lock_guard<std::mutex> lock(routeMutex);
        routeExpiry = {};
    }
    assert(chooseRoute(proxy, false, "/unused-test-ca.pem", &selected));
    assert(selected == AuthRoute::AliDoh); // 代理配置不可解析时仍可探测直连。
    const unsigned char body[] = "{}";
    MaiChatAuthHttpResult result{};
    maiChatAuthHttpPerform("https://ichat.life/api/v1/auth/refresh", "POST", nullptr,
                           body, 2, "/unused-test-ca.pem", &result);
    assert(result.transportCode == CURLE_OPERATION_TIMEDOUT && sends == 1);
    maiChatAuthHttpFreeResult(&result);
    std::cout << "iOS auth route selection and single POST: PASS\n";
}
