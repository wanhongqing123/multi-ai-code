// One bounded HTTPS download shared by Desktop IM image, file, video and voice messages.
#pragma once

#include <QByteArray>
#include <QNetworkProxy>
#include <QString>
#include <QUrl>

#include <memory>

#include "MaiCurlDownload.h"

struct DesktopCurlAttachmentResult {
    enum class Status { Downloaded, InvalidUrl, NetworkError, WriteError, Canceled };
    Status status = Status::NetworkError;
    int transferCode = 0;
    qint64 bytes = 0;
    bool ok() const { return status == Status::Downloaded; }
};

class DesktopCurlAttachmentDownload final {
public:
    // The caller runs this synchronous operation off the UI thread. A nonempty CA PEM is
    // used by local TLS tests; the production path uses the bundled trust roots on macOS.
    static DesktopCurlAttachmentResult perform(
        const QUrl& url, const QString& targetPath, qint64 maximumBytes,
        const QByteArray& certificatePem, const QNetworkProxy& proxy,
        const std::shared_ptr<MaiDownloadCancel>& canceled);
};
