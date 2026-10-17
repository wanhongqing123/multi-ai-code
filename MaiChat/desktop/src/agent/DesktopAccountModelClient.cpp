// 文件作用：每次模型请求读取当前账号 Key，按模型名选供应商地址；不经模型代理。
#include "agent/DesktopAccountModelClient.h"

#include <utility>

#include "MaiOpenAiClient.h"

namespace {

std::string toUtf8(const QString& value) {
    const QByteArray bytes = value.toUtf8();
    return std::string(bytes.constData(), static_cast<std::size_t>(bytes.size()));
}

}  // namespace

DesktopAccountModelClient::DesktopAccountModelClient(Config config)
    : config_(std::move(config)) {}

MaiWireApi DesktopAccountModelClient::wireApi() const {
    return MaiWireApi::Responses;
}

MaiError DesktopAccountModelClient::stream(const MaiModelRequest& request,
                                           const MaiStreamSink& sink,
                                           const std::atomic<bool>& cancel) {
    const QString model = QString::fromUtf8(request.model.c_str(),
                                           static_cast<int>(request.model.size()));
    QString provider;
    QString baseUrl;
    if (model == QStringLiteral("glm-5.3") || model == QStringLiteral("glm-5.3-flash")) {
        provider = QStringLiteral("glm");
        baseUrl = config_.glmBaseUrl;
    } else if (model == QStringLiteral("deepseek-flash")) {
        provider = QStringLiteral("deepseek");
        baseUrl = config_.deepseekBaseUrl;
    } else {
        return MaiError::make(MaiErrorCode::InvalidInput, "unsupported account model");
    }
    if (!config_.keyProvider || baseUrl.isEmpty())
        return MaiError::make(MaiErrorCode::NotConfigured, "account model unavailable");
    const QString key = config_.keyProvider(provider);
    if (key.isEmpty())
        return MaiError::make(MaiErrorCode::NotConfigured, "account model key unavailable");
    MaiModelConfig config;
    config.baseUrl = toUtf8(baseUrl);
    config.apiKey = toUtf8(key);
    config.wire = MaiWireApi::Responses;
    config.prepareImage = config_.prepareImage;
    auto client = makeMaiModelClient(config);
    if (!client) return MaiError::make(MaiErrorCode::Internal, "model client creation failed");
    return client->stream(request, sink, cancel);
}
