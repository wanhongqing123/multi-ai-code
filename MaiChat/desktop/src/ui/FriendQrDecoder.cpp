#include "ui/FriendQrDecoder.h"

#include <QImage>
#include <QRegularExpression>
#include <QUrl>
#include <QUrlQuery>
#include <algorithm>
#include <cstring>
#include <memory>

extern "C" {
#include <quirc.h>
}

QString maiChatAccountIdFromQrImage(const QImage& image) {
    if (image.isNull() || image.width() <= 0 || image.height() <= 0 ||
        image.width() > 4096 || image.height() > 4096) return {};
    const QImage grayscale = image.convertToFormat(QImage::Format_Grayscale8);
    std::unique_ptr<quirc, decltype(&quirc_destroy)> decoder(quirc_new(), quirc_destroy);
    if (!decoder || quirc_resize(decoder.get(), grayscale.width(), grayscale.height()) != 0)
        return {};
    int width = 0, height = 0;
    uint8_t* pixels = quirc_begin(decoder.get(), &width, &height);
    if (!pixels || width != grayscale.width() || height != grayscale.height()) return {};
    for (int row = 0; row < height; ++row)
        std::memcpy(pixels + row * width, grayscale.constScanLine(row), width);
    quirc_end(decoder.get());
    static const QRegularExpression accountPattern(QStringLiteral("^[a-z][a-z0-9_-]{2,31}$"));
    for (int index = 0; index < std::min(quirc_count(decoder.get()), 16); ++index) {
        quirc_code code;
        quirc_data data;
        quirc_extract(decoder.get(), index, &code);
        if (quirc_decode(&code, &data) != QUIRC_SUCCESS) {
            quirc_flip(&code);
            if (quirc_decode(&code, &data) != QUIRC_SUCCESS) continue;
        }
        const QByteArray payload(reinterpret_cast<const char*>(data.payload), data.payload_len);
        const QString text = QString::fromUtf8(payload);
        if (text.toUtf8() != payload) continue;
        const QUrl url(text, QUrl::StrictMode);
        if (!url.isValid() || url.scheme() != QStringLiteral("maichat") ||
            url.host() != QStringLiteral("friend") || url.path() != QStringLiteral("/add") ||
            !url.userName().isEmpty() || url.port() != -1 || !url.fragment().isEmpty()) continue;
        const QUrlQuery query(url);
        if (query.queryItems().size() != 1 || !query.hasQueryItem(QStringLiteral("user_id")))
            continue;
        const QString accountId = query.queryItemValue(QStringLiteral("user_id"));
        if (accountPattern.match(accountId).hasMatch()) return accountId;
    }
    return {};
}
