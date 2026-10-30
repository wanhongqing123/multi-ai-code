// 文件作用：按当前会话选定的主模型把 Responses 请求直连对应供应商。
// 账号 Key 回调在模型工作线程调用；宿主须保证回调线程安全且不记录密钥。
#pragma once

#include <QString>
#include <functional>

#include "MaiModelClient.h"

class DesktopAccountModelClient final : public MaiModelClient {
public:
    struct Config {
        QString glmBaseUrl;
        QString deepseekBaseUrl;
        std::function<QString(const QString&)> keyProvider;
        MaiModelImagePreparer prepareImage;
        // 测试可注入确定的逐跳网络路由；正式 Mac 配置为空时查询系统代理。
        std::function<bool(const std::string&, MaiModelProxy&)> proxyResolver;
    };

    explicit DesktopAccountModelClient(Config config);
    MaiError stream(const MaiModelRequest& request, const MaiStreamSink& sink,
                    const std::atomic<bool>& cancel) override;
    MaiWireApi wireApi() const override;

private:
    Config config_;
};
