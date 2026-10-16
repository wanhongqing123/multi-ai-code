// 文件作用：声明 iOS 自有 HTTP 请求共用的 libcurl/OpenSSL 传输接口。
// 资源约定：每个请求结果必须释放；取消标记由调用方持有到请求返回后。
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MaiChatHttpCancellation MaiChatHttpCancellation;

typedef struct MaiChatHttpResult {
    uint8_t* responseBody;
    size_t responseLength;
    char* redirectLocation;
    long statusCode;
    int transportCode;
} MaiChatHttpResult;

MaiChatHttpCancellation* maiChatHttpCreateCancellation(void);
void maiChatHttpCancel(MaiChatHttpCancellation* cancellation);
void maiChatHttpFreeCancellation(MaiChatHttpCancellation* cancellation);
// 仅支持公开 GET 和 JSON POST。HTTPS 始终验证 CA 和域名，不自动跟随跳转。
// HTTP 仅允许没有凭据的公开 GET，以兼容既有 IM 头像地址。
// transportCode=0 表示已收到 HTTP 响应；POST 的 4xx/5xx 正文过大时只返回状态、丢弃正文。
// 成功响应正文过大仍返回限额错误；负值表示本地错误，正值为 CURLcode。
void maiChatHttpPerform(const char* url, const char* method, const char* bearer, const char* contentType,
                        const uint8_t* requestBody, size_t requestLength, size_t maximumResponseBytes,
                        long timeoutMilliseconds, const char* caBundlePath,
                        MaiChatHttpCancellation* cancellation, MaiChatHttpResult* result);
void maiChatHttpFreeResult(MaiChatHttpResult* result);

#ifdef __cplusplus
}
#endif
