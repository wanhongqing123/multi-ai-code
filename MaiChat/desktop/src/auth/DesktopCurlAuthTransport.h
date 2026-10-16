// 文件作用：定义桌面账号请求的一次性 libcurl 传输接口，不保存会话或凭据。
// 线程约定：perform 在工作线程同步执行；调用方负责把结果送回 Qt 所属线程。
// 安全边界：仅允许正式 HTTPS 账号地址或本机测试地址；失败不重试 POST。
#pragma once

#include <QByteArray>
#include <QString>
#include <QUrl>

#include <atomic>
#include <memory>

struct DesktopCurlAuthResult {
    int status = 0;
    QByteArray body;
    QString error;
    bool responseTooLarge = false;
};

class DesktopCurlAuthTransport final {
public:
    // 发送一次 GET/POST；返回的状态码与响应正文仅属于此次调用。
    // cancelled 可在对象关闭时由调用方置位，未收到确认的 POST 不会重放。
    static DesktopCurlAuthResult perform(const QUrl& url, const QByteArray& method, const QByteArray& body,
                                         const QByteArray& bearer, const std::shared_ptr<std::atomic_bool>& cancelled);
};
