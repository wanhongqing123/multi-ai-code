// 文件作用：声明 iOS 账号请求的 libcurl C 接口，供 Swift 账号客户端直接调用。
// 资源约定：调用方必须对每次请求结果调用 maiChatAuthHttpFreeResult，释放响应缓冲区。
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MaiChatAuthHttpResult {
    uint8_t* responseBody;
    size_t responseLength;
    long statusCode;
    int transportCode;
} MaiChatAuthHttpResult;

// 只接受 GET/POST 和 HTTP(S)。HTTPS 必须传入 CA 文件，且始终校验服务端证书与域名。
// transportCode 为 0 表示收到了 HTTP 响应；-1/-2/-3/-4 分别表示参数错误、响应超限、
// libcurl 初始化失败、内存不足；其他正数与 libcurl CURLcode 一致。
void maiChatAuthHttpPerform(const char* url, const char* method, const char* bearer, const uint8_t* requestBody,
                            size_t requestLength, const char* caBundlePath, MaiChatAuthHttpResult* result);

void maiChatAuthHttpFreeResult(MaiChatAuthHttpResult* result);

// 为第一方 HTTPS 请求预先探测一条可用线路；只允许 https://ichat.life/。
// 返回 -1 表示不可用，0 使用系统路由，1/2 分别使用阿里/Cloudflare DoH。
// 同步探测最多约 9.5 秒；排队及系统线程调度可能延长等待。调用方应在工作线程
// 调用，并于调用前后检查取消；探测本身不支持中途取消，context 预留且不读取。
int maiChatSelectFirstPartyRoute(const char* url, const char* caPath, void* context);
// 传输失败后使已缓存线路失效；请求本身不得因此被自动重放。
void maiChatInvalidateFirstPartyRoute(void* context);

#ifdef __cplusplus
}
#endif
