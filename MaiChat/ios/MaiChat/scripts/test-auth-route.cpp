// 定向验证 iOS 账号线路选择和写请求语义；替换网络结果，不访问真实账号或凭据。
#define MAICHAT_AUTH_ROUTE_TEST 1
#include "../MaiChat/IM/MaiChatAuthTransport.cpp"

#include <atomic>
#include <cassert>
#include <iostream>
#include <vector>

static std::atomic_int systemCount{0}, aliCount{0}, cloudCount{0}, sends{0}, delayMs{0};
static std::atomic_bool systemWorks{false}, aliWorks{true};

static bool fakeProbe(AuthRoute route) {
    if (delayMs > 0) std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
    if (route == AuthRoute::System) { ++systemCount; return systemWorks.load(); }
    if (route == AuthRoute::AliDoh) { ++aliCount; return aliWorks.load(); }
    ++cloudCount;
    return false;
}

static CURLcode fakeBusiness() { ++sends; return CURLE_OPERATION_TIMEDOUT; }

int main(int argc, char** argv) {
    if (argc == 5 && std::string(argv[1]) == "--quic-write") {
        // 经过真实账号传输入口发送测试 POST；连接到本机但仍校验原域名和测试 CA。
        // 服务端记录每次收到的正文，用于验证失败与降级不会重放业务写入。
        const std::string mapping = std::string("ichat.life:443:127.0.0.1:") + argv[2];
        testConnectTo = curl_slist_append(nullptr, mapping.c_str());
        testForceDirect = true;
        systemWorks = true;
        probeHook = fakeProbe;
        const std::string body = argv[4];
        MaiChatAuthHttpResult result{};
        maiChatAuthHttpPerform("https://ichat.life/api/v1/auth/refresh", "POST", nullptr,
            reinterpret_cast<const uint8_t*>(body.data()), body.size(), argv[3], &result);
        const bool failureExpected = body == "ambiguous-fixture-body" || body == "untrusted-fixture-body";
        assert(failureExpected ? result.transportCode != 0 : result.transportCode == 0 && result.statusCode == 200);
        maiChatAuthHttpFreeResult(&result);
        curl_slist_free_all(testConnectTo);
        std::cout << "iOS auth QUIC write: PASS\n";
        return 0;
    }
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
    // 共享入口必须自行初始化 curl，并拒绝冒充账号 origin 的地址和缺失 CA。
    assert(maiChatSelectFirstPartyRoute("https://ichat.life.evil/api/v1/", "/unused-test-ca.pem", nullptr) == -1);
    assert(maiChatSelectFirstPartyRoute("http://ichat.life/api/v1/", "/unused-test-ca.pem", nullptr) == -1);
    assert(maiChatSelectFirstPartyRoute("https://ichat.life@evil/api/v1/", "/unused-test-ca.pem", nullptr) == -1);
    assert(maiChatSelectFirstPartyRoute("https://ichat.life/api/v1/", "", nullptr) == -1);
    systemWorks = false;
    assert(maiChatSelectFirstPartyRoute("https://ichat.life/api/v1/cloud-drive", "/unused-test-ca.pem", nullptr) == 1);
    maiChatInvalidateFirstPartyRoute(nullptr);
    aliWorks = false;
    assert(maiChatSelectFirstPartyRoute("https://ichat.life/api/v1/cloud-drive", "/unused-test-ca.pem", nullptr) == -1);
    // 并发失败应合并一次系统与两条 DoH 探测，而非逐个等待所有线路超时。
    maiChatInvalidateFirstPartyRoute(nullptr);
    delayMs = 50;
    systemCount = 0; aliCount = 0; cloudCount = 0;
    std::vector<std::thread> callers;
    for (int i = 0; i < 12; ++i) callers.emplace_back([proxy] {
        AuthRoute picked = AuthRoute::System;
        assert(!chooseRoute(proxy, true, "/concurrent-test-ca.pem", &picked));
    });
    for (auto& caller : callers) caller.join();
    assert(systemCount == 1 && aliCount == 1 && cloudCount == 1);
    delayMs = 0;
    std::cout << "iOS auth route selection and single POST: PASS\n";
}
