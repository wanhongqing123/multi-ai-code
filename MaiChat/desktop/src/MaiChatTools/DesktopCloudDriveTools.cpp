#include "MaiChatTools/DesktopCloudDriveTools.h"

#include "MaiCloudDriveClient.h"
#include "MaiCloudDriveTools.h"
#include "MaiCloudVod.h"
#include "MaiCurlDownload.h"
#include "ui/DesktopCloudVod.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QPointer>
#include <QResource>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>
#include <QUuid>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

static void initializeCloudDriveAuthCertificates() {
    Q_INIT_RESOURCE(AuthCertificates);
}

namespace {

constexpr quint64 kMaxVideoBytes = 2'000'000'000ULL;
constexpr quint64 kMaxImageBytes = 20'000'000ULL;

QString suffixForContentType(const QString& contentType) {
    if (contentType == QStringLiteral("video/mp4")) return QStringLiteral("mp4");
    if (contentType == QStringLiteral("video/quicktime")) return QStringLiteral("mov");
    if (contentType == QStringLiteral("image/jpeg")) return QStringLiteral("jpg");
    if (contentType == QStringLiteral("image/png")) return QStringLiteral("png");
    if (contentType == QStringLiteral("image/webp")) return QStringLiteral("webp");
    if (contentType == QStringLiteral("image/heic")) return QStringLiteral("heic");
    return {};
}

struct ActiveTransfer {
    QString account;
    QString checkpoint;
    QString name;
    qint64 size = 0;
    std::shared_ptr<MaiCloudDriveCancel> cancel;
    bool uploading = false;
};

class ActiveTransfers {
public:
    ~ActiveTransfers() {
        cancelAll();
        std::vector<Worker> pending;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pending.swap(workers_);
        }
        for (Worker& worker : pending) {
            if (worker.thread.joinable()) worker.thread.join();
        }
    }

    bool begin(const ActiveTransfer& transfer) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (transfers_.contains(transfer.checkpoint)) return false;
        transfers_.insert(transfer.checkpoint, transfer);
        return true;
    }

    void end(const QString& checkpoint) {
        std::lock_guard<std::mutex> lock(mutex_);
        transfers_.remove(checkpoint);
    }

    void launch(std::function<void()> work) {
        std::vector<Worker> finished;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            finished.reserve(workers_.size());
            auto it = workers_.begin();
            while (it != workers_.end()) {
                if (it->done->load(std::memory_order_acquire)) {
                    finished.push_back(std::move(*it));
                    it = workers_.erase(it);
                } else {
                    ++it;
                }
            }
        }
        for (Worker& worker : finished) {
            if (worker.thread.joinable()) worker.thread.join();
        }
        const auto done = std::make_shared<std::atomic_bool>(false);
        std::lock_guard<std::mutex> lock(mutex_);
        workers_.emplace_back();
        try {
            workers_.back().done = done;
            workers_.back().thread = std::thread([work = std::move(work), done] {
                try { work(); } catch (...) { /* The next list shows any retained checkpoint. */ }
                done->store(true, std::memory_order_release);
            });
        } catch (...) {
            workers_.pop_back();
            throw;
        }
    }

    void cancelAll() {
        QList<std::shared_ptr<MaiCloudDriveCancel>> pending;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const ActiveTransfer& transfer : transfers_) pending.append(transfer.cancel);
        }
        for (const auto& cancel : pending) mai_cloud_drive_cancel_request(cancel.get());
    }

    void cancelAccountsExcept(const QString& account) {
        QList<std::shared_ptr<MaiCloudDriveCancel>> pending;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const ActiveTransfer& transfer : transfers_) {
                if (transfer.account != account) pending.append(transfer.cancel);
            }
        }
        for (const auto& cancel : pending) mai_cloud_drive_cancel_request(cancel.get());
    }

    bool contains(const QString& checkpoint) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return transfers_.contains(checkpoint);
    }

    bool isUploading(const QString& checkpoint) const {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = transfers_.constFind(checkpoint);
        return it != transfers_.cend() && it->uploading;
    }

    bool pause(const QString& checkpoint) {
        std::shared_ptr<MaiCloudDriveCancel> cancel;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto it = transfers_.constFind(checkpoint);
            if (it == transfers_.cend() || !it->uploading) return false;
            cancel = it->cancel;
        }
        mai_cloud_drive_cancel_request(cancel.get());
        return true;
    }

    QList<ActiveTransfer> uploadsIn(const QString& directory) const {
        QList<ActiveTransfer> found;
        std::lock_guard<std::mutex> lock(mutex_);
        for (const ActiveTransfer& transfer : transfers_) {
            if (transfer.uploading && QFileInfo(transfer.checkpoint).absolutePath() == directory)
                found.append(transfer);
        }
        return found;
    }

private:
    struct Worker {
        std::thread thread;
        std::shared_ptr<std::atomic_bool> done;
    };
    mutable std::mutex mutex_;
    QHash<QString, ActiveTransfer> transfers_;
    std::vector<Worker> workers_;
};

class TransferRegistration {
public:
    TransferRegistration(std::shared_ptr<ActiveTransfers> transfers, ActiveTransfer transfer)
        : transfers_(std::move(transfers)), checkpoint_(transfer.checkpoint),
          registered_(transfers_->begin(transfer)) {}
    ~TransferRegistration() { if (registered_) transfers_->end(checkpoint_); }
    bool registered() const { return registered_; }

private:
    std::shared_ptr<ActiveTransfers> transfers_;
    QString checkpoint_;
    bool registered_;
};

MaiToolResult failure(MaiErrorCode code, const char* message) {
    return MaiToolResult::failure(code, message);
}

MaiErrorCode errorCode(const MaiCloudDriveResult& result) {
    if (result.status == MAI_CLOUD_DRIVE_CANCELED) return MaiErrorCode::Canceled;
    if (result.status == MAI_CLOUD_DRIVE_INVALID_ARGUMENT || result.status == MAI_CLOUD_DRIVE_CHECKPOINT_MISMATCH ||
        result.status == MAI_CLOUD_DRIVE_SOURCE_NOT_SEEKABLE)
        return MaiErrorCode::InvalidInput;
    if (result.http_status == 404) return MaiErrorCode::NotFound;
    if (result.status == MAI_CLOUD_DRIVE_NETWORK_ERROR) return MaiErrorCode::Network;
    return MaiErrorCode::Internal;
}

MaiToolResult errorResult(const MaiCloudDriveResult& result) {
    if (result.http_status == 401) return failure(MaiErrorCode::NotConfigured, "MaiChat account login has expired");
    if (result.http_status == 404 || result.http_status == 503)
        return failure(MaiErrorCode::NotConfigured, "the private cloud drive is unavailable");
    if (result.http_status == 413)
        return failure(MaiErrorCode::InvalidInput,
                       "the video is larger than 2 GB or the account's 10 GB quota is full");
    if (result.http_status == 410)
        return failure(MaiErrorCode::NotFound, "the upload session has expired; start a new upload");
    if (result.status == MAI_CLOUD_DRIVE_CHECKPOINT_MISMATCH)
        return failure(MaiErrorCode::InvalidInput, "the source video no longer matches the paused upload");
    if (result.status == MAI_CLOUD_DRIVE_CANCELED)
        return failure(MaiErrorCode::Canceled, "the operation was canceled; an upload checkpoint was kept");
    return failure(errorCode(result), "the private cloud drive operation failed");
}

QByteArray trustedRoots(const DesktopVideoCircleAccess& access) {
    if (!access.caPem.isEmpty()) return access.caPem;
    initializeCloudDriveAuthCertificates();
    QFile certificates(QStringLiteral(":/maichat/auth/cacert.pem"));
    return certificates.open(QIODevice::ReadOnly) ? certificates.readAll() : QByteArray();
}

QString accountDirectory(const QString& account) {
    const QString root = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (root.isEmpty() || account.isEmpty()) return {};
    const QString hash =
        QString::fromLatin1(QCryptographicHash::hash(account.toUtf8(), QCryptographicHash::Sha256).toHex());
    const QString path = QDir(root).filePath(QStringLiteral("CloudDrive/") + hash);
    return QDir().mkpath(path) ? path : QString();
}

QString playbackDirectory() {
    const QString root = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (root.isEmpty()) return {};
    const QString path = QDir(root).filePath(QStringLiteral("CloudDrivePlayback"));
    return QDir().mkpath(path) ? path : QString();
}

struct Operation {
    DesktopVideoCircleAccess access;
    QByteArray base;
    QByteArray token;
    QByteArray proxy;
    QByteArray suffix;
    QByteArray roots;
    std::shared_ptr<MaiCloudDriveCancel> cancel;
    std::shared_ptr<MaiDownloadCancel> downloadCancel;
    std::atomic_bool done{false};
    std::thread watcher;

    Operation(DesktopVideoCircleAccess current, const std::function<DesktopVideoCircleAccess()>& provider,
              const MaiToolContext& context, std::shared_ptr<MaiCloudDriveCancel> sharedCancel = {})
        : access(std::move(current)),
          base(access.authBaseUrl.toUtf8()),
          token(access.accountToken.toUtf8()),
          proxy(access.proxyUrl.toUtf8()),
          suffix(access.signedUploadHostSuffix.toUtf8()),
          roots(trustedRoots(access)),
          cancel(sharedCancel ? std::move(sharedCancel) : std::shared_ptr<MaiCloudDriveCancel>(
              mai_cloud_drive_cancel_create(), mai_cloud_drive_cancel_destroy)),
          downloadCancel(mai_download_cancel_create(), mai_download_cancel_destroy) {
        if (!cancel || !downloadCancel) return;
        const std::atomic<bool>* turnCancel = context.cancel;
        const QString account = access.accountId;
        watcher = std::thread([this, provider, turnCancel, account] {
            while (!done.load(std::memory_order_relaxed)) {
                if ((turnCancel && turnCancel->load(std::memory_order_relaxed)) || !provider ||
                    provider().accountId != account) {
                    mai_cloud_drive_cancel_request(cancel.get());
                    mai_download_cancel_request(downloadCancel.get());
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        });
    }

    ~Operation() {
        done.store(true, std::memory_order_relaxed);
        if (watcher.joinable()) watcher.join();
    }

    MaiCloudDriveOptions options() const {
        MaiCloudDriveOptions options{};
        options.auth_base_url_utf8 = base.constData();
        options.account_token_utf8 = token.constData();
        options.ca_pem = roots.isEmpty() ? nullptr : roots.constData();
        options.ca_pem_length = size_t(roots.size());
        options.proxy_url_utf8 = access.proxyUrl.isNull() ? nullptr : proxy.constData();
        options.signed_upload_host_suffix_utf8 = suffix.isEmpty() ? nullptr : suffix.constData();
        return options;
    }
};

MaiToolResult resultJson(const MaiCloudDriveResult& result) {
    if (result.status != MAI_CLOUD_DRIVE_OK) return errorResult(result);
    const QByteArray body(result.response_json_utf8, int(result.response_bytes));
    const QJsonDocument document = QJsonDocument::fromJson(body);
    if (!document.isObject()) return failure(MaiErrorCode::Protocol, "invalid private cloud drive response");
    return MaiToolResult::success(body.toStdString());
}

MaiToolResult request(Operation& operation, MaiCloudDriveAction action, const QJsonObject& payload) {
    const QByteArray body = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    MaiCloudDriveResult result{};
    const MaiCloudDriveOptions options = operation.options();
    mai_cloud_drive_request(&options, action, body.constData(), operation.cancel.get(), &result);
    const MaiToolResult reply = result.http_status == 404 &&
        action >= MAI_CLOUD_DRIVE_LIST_FOLDERS && action <= MAI_CLOUD_DRIVE_RENAME_MEDIA ?
        failure(MaiErrorCode::NotFound, "cloud folder or media was not found") :
        result.http_status == 400 && action == MAI_CLOUD_DRIVE_RENAME_MEDIA ?
        failure(MaiErrorCode::InvalidInput, "use a safe file name and keep the existing file extension") :
        resultJson(result);
    mai_cloud_drive_result_free(&result);
    return reply;
}

QJsonObject parseObject(const std::string& text) {
    return QJsonDocument::fromJson(QByteArray::fromStdString(text)).object();
}

QJsonObject checkpointByTask(const QString& directory, const QString& taskId) {
    MaiCloudDriveResult result{};
    const QByteArray dir = directory.toUtf8();
    mai_cloud_drive_list_local_checkpoints(dir.constData(), &result);
    QJsonObject selected;
    if (result.status == MAI_CLOUD_DRIVE_OK) {
        const QJsonArray uploads =
            QJsonDocument::fromJson(QByteArray(result.response_json_utf8, int(result.response_bytes)))
                .object()
                .value(QStringLiteral("uploads"))
                .toArray();
        for (const QJsonValue& entry : uploads) {
            const QJsonObject upload = entry.toObject();
            const QString path = upload.value(QStringLiteral("checkpoint_path")).toString();
            if (QFileInfo(path).absolutePath() == directory &&
                QFileInfo(path).fileName() == taskId + QStringLiteral(".maicloud.json")) {
                selected = upload;
                break;
            }
        }
    }
    mai_cloud_drive_result_free(&result);
    return selected;
}

MaiToolResult pendingUploads(const QString& directory, const std::shared_ptr<ActiveTransfers>& transfers) {
    MaiCloudDriveResult result{};
    const QByteArray dir = directory.toUtf8();
    mai_cloud_drive_list_local_checkpoints(dir.constData(), &result);
    if (result.status != MAI_CLOUD_DRIVE_OK) {
        const MaiToolResult error = errorResult(result);
        mai_cloud_drive_result_free(&result);
        return error;
    }
    const QJsonObject body =
        QJsonDocument::fromJson(QByteArray(result.response_json_utf8, int(result.response_bytes))).object();
    mai_cloud_drive_result_free(&result);
    QJsonArray safe;
    QHash<QString, bool> seen;
    for (const QJsonValue& entry : body.value(QStringLiteral("uploads")).toArray()) {
        const QJsonObject upload = entry.toObject();
        const QString path = upload.value(QStringLiteral("checkpoint_path")).toString();
        if (QFileInfo(path).absolutePath() != directory) continue;
        const QString file = QFileInfo(path).fileName();
        if (!file.endsWith(QStringLiteral(".maicloud.json"))) continue;
        seen.insert(path, true);
        safe.push_back(QJsonObject{{QStringLiteral("task_id"), file.left(file.size() - 14)},
                                   {QStringLiteral("name"), upload.value(QStringLiteral("name"))},
                                   {QStringLiteral("size_bytes"), upload.value(QStringLiteral("size_bytes"))},
                                   {QStringLiteral("state"), transfers->isUploading(path)
                                       ? QStringLiteral("uploading") : QStringLiteral("paused")}});
    }
    for (const ActiveTransfer& active : transfers->uploadsIn(directory)) {
        if (seen.contains(active.checkpoint)) continue;
        const QString file = QFileInfo(active.checkpoint).fileName();
        if (!file.endsWith(QStringLiteral(".maicloud.json"))) continue;
        safe.push_back(QJsonObject{{QStringLiteral("task_id"), file.left(file.size() - 14)},
                                   {QStringLiteral("name"), active.name},
                                   {QStringLiteral("size_bytes"), active.size},
                                   {QStringLiteral("state"), QStringLiteral("uploading")}});
    }
    return MaiToolResult::success(
        QJsonDocument(QJsonObject{{QStringLiteral("uploads"), safe}}).toJson(QJsonDocument::Compact).toStdString());
}

MaiToolResult upload(Operation& operation, const QString& source, const QString& checkpoint, const QString& name) {
    MaiCloudDriveMediaInfo media{};
    const QByteArray display = name.toUtf8();
    if (!mai_cloud_drive_media_info(display.constData(), &media))
        return failure(MaiErrorCode::InvalidInput, "unsupported cloud media type");
    const QByteArray base = source.toUtf8();
    const QByteArray task = checkpoint.toUtf8();
    const QByteArray mime(media.content_type_utf8);
    MaiCloudDriveResult result{};
    const MaiCloudDriveOptions options = operation.options();
    mai_cloud_drive_upload_resumable(&options, base.constData(), task.constData(), display.constData(),
                                     mime.constData(), operation.cancel.get(), nullptr, nullptr, &result);
    if (result.status != MAI_CLOUD_DRIVE_OK) {
        const MaiToolResult error = errorResult(result);
        mai_cloud_drive_result_free(&result);
        return error;
    }
    const QJsonObject body =
        QJsonDocument::fromJson(QByteArray(result.response_json_utf8, int(result.response_bytes))).object();
    mai_cloud_drive_result_free(&result);
    const QString mediaId = body.value(QStringLiteral("video_id")).toString();
    if (mediaId.isEmpty()) return failure(MaiErrorCode::Protocol, "upload completed without a media ID");
    return MaiToolResult::success(QJsonDocument(QJsonObject{{QStringLiteral("media_id"), mediaId},
                                                            {QStringLiteral("status"), QStringLiteral("ready")}})
                                      .toJson(QJsonDocument::Compact)
                                      .toStdString());
}

MaiToolResult queueUpload(const DesktopVideoCircleAccess& access, const QString& source,
                          const QString& checkpoint, const QString& name,
                          const std::shared_ptr<ActiveTransfers>& transfers) {
    const auto cancel = std::shared_ptr<MaiCloudDriveCancel>(
        mai_cloud_drive_cancel_create(), mai_cloud_drive_cancel_destroy);
    if (!cancel) return failure(MaiErrorCode::Internal, "could not start the upload task");
    const QString taskId = QFileInfo(checkpoint).fileName().chopped(14);
    if (!transfers->begin(ActiveTransfer{
            access.accountId, checkpoint, name, QFileInfo(source).size(), cancel, true}))
        return failure(MaiErrorCode::Busy, "the upload task is already active");
    try {
        // ActiveTransfers owns and joins the thread. The worker captures only
        // value data and a shared cancel token; it never borrows turn context.
        ActiveTransfers* owner = transfers.get();
        transfers->launch([owner, access, source, checkpoint, name, cancel] {
            try {
                MaiToolContext backgroundContext;
                const std::function<DesktopVideoCircleAccess()> fixedAccess = [access] { return access; };
                Operation operation(access, fixedAccess, backgroundContext, cancel);
                if (operation.cancel && operation.downloadCancel)
                    (void)upload(operation, source, checkpoint, name);
            } catch (...) {
                // The checkpoint, when present, remains available for resume.
            }
            owner->end(checkpoint);
        });
    } catch (const std::exception&) {
        transfers->end(checkpoint);
        return failure(MaiErrorCode::Internal, "could not queue the upload task");
    }
    return MaiToolResult::success(QJsonDocument(QJsonObject{
        {QStringLiteral("accepted"), true}, {QStringLiteral("task_id"), taskId},
        {QStringLiteral("state"), QStringLiteral("uploading")}})
        .toJson(QJsonDocument::Compact).toStdString());
}

}  // namespace

void registerDesktopCloudDriveTools(MaiToolRegistry& registry, std::function<DesktopVideoCircleAccess()> accessProvider,
                                    QObject* presenter,
                                    std::function<bool(const QString&, bool isImage,
                                                       std::shared_ptr<MaiCloudVod>)> openMedia,
                                    std::function<DesktopSessionRoom(const QString&)> roomProvider) {
    QPointer<QObject> target(presenter);
    auto transfers = std::make_shared<ActiveTransfers>();
    if (presenter) {
        auto* monitor = new QTimer(presenter);
        monitor->setInterval(100);
        QObject::connect(monitor, &QTimer::timeout, presenter,
                         [provider = accessProvider, transfers] {
            const DesktopVideoCircleAccess current = provider ? provider() : DesktopVideoCircleAccess{};
            transfers->cancelAccountsExcept(current.accountId);
        });
        QObject::connect(presenter, &QObject::destroyed,
                         [transfers] { transfers->cancelAll(); });
        monitor->start();
    }
    registerMaiCloudDriveTools(
        registry,
        [provider = std::move(accessProvider), target, open = std::move(openMedia),
         roomProvider = std::move(roomProvider), transfers](
            const std::string& name, const std::string& argumentsJson, const MaiToolContext& context) -> MaiToolResult {
            const DesktopVideoCircleAccess access = provider ? provider() : DesktopVideoCircleAccess{};
            if (!access.isValid() || target.isNull())
                return failure(MaiErrorCode::NotConfigured, "sign in to MaiChat to use the private cloud drive");
            if (name == "maichat_session_attachment_open") {
                const QJsonObject args = parseObject(argumentsJson);
                const QString session = args.value(QStringLiteral("session_id")).toString();
                const QString id = args.value(QStringLiteral("attachment_id")).toString();
                if (!roomProvider || session != QString::fromStdString(context.sessionId) ||
                    context.root.empty())
                    return failure(MaiErrorCode::InvalidInput,
                        "Session attachment must belong to the current workspace");
                const DesktopSessionRoom room = roomProvider(session);
                if (room.login.isEmpty())
                    return failure(MaiErrorCode::NotConfigured,
                        "Session sync login is unavailable");
                const auto downloaded = DesktopSessionAttachments::download(access, room, id);
                if (!downloaded.ok())
                    return failure(MaiErrorCode::Network,
                        downloaded.error.toUtf8().toStdString().c_str());
                const QString root = QString::fromStdString(context.root);
                const QString relative = QStringLiteral(".maichat/session-attachments/") +
                    QUuid::createUuid().toString(QUuid::WithoutBraces).left(8) +
                    QLatin1Char('-') +
                    downloaded.metadata.value(QStringLiteral("name")).toString();
                const QString destination = QDir(root).filePath(relative);
                if (!QDir().mkpath(QFileInfo(destination).absolutePath()) ||
                    !QFile::copy(downloaded.path, destination) ||
                    context.resolvePath(relative.toUtf8().toStdString()).empty()) {
                    QFile::remove(destination);
                    QFile::remove(downloaded.path);
                    return failure(MaiErrorCode::Internal,
                        "could not retain the Session attachment in the workspace");
                }
                QFile::remove(downloaded.path);
                const QJsonObject output{
                    {QStringLiteral("path"), relative},
                    {QStringLiteral("mime_type"),
                     downloaded.metadata.value(QStringLiteral("content_type"))}};
                return MaiToolResult::success(
                    QJsonDocument(output).toJson(QJsonDocument::Compact).toStdString());
            }
            Operation operation(access, provider, context);
            if (!operation.cancel || !operation.downloadCancel)
                return failure(MaiErrorCode::Internal, "could not start cloud drive operation");
            const QJsonObject args = parseObject(argumentsJson);
            if (name == "maichat_cloud_drive_list_folders")
                return request(operation, MAI_CLOUD_DRIVE_LIST_FOLDERS, args);
            if (name == "maichat_cloud_drive_create_folder")
                return request(operation, MAI_CLOUD_DRIVE_CREATE_FOLDER, args);
            if (name == "maichat_cloud_drive_update_folder")
                return request(operation, MAI_CLOUD_DRIVE_UPDATE_FOLDER, args);
            if (name == "maichat_cloud_drive_delete_folder")
                return request(operation, MAI_CLOUD_DRIVE_DELETE_FOLDER, args);
            if (name == "maichat_cloud_drive_move_media")
                return request(operation, MAI_CLOUD_DRIVE_MOVE_MEDIA, args);
            if (name == "maichat_cloud_drive_rename_media")
                return request(operation, MAI_CLOUD_DRIVE_RENAME_MEDIA, args);
            const QString directory = accountDirectory(access.accountId);
            if (directory.isEmpty()) return failure(MaiErrorCode::Internal, "could not open private upload storage");
            if (name == "maichat_cloud_drive_list_media") {
                const MaiToolResult listed = request(operation, MAI_CLOUD_DRIVE_LIST_VIDEOS, args);
                if (listed.hasError()) return listed;
                QJsonObject body = parseObject(listed.output());
                QJsonArray media;
                for (const QJsonValue& entry : body.value(QStringLiteral("videos")).toArray()) {
                    QJsonObject item = entry.toObject();
                    const QString id = item.value(QStringLiteral("id")).toString();
                    if (id.isEmpty())
                        return failure(MaiErrorCode::Protocol, "cloud media list contains an invalid ID");
                    item.remove(QStringLiteral("id"));
                    item.insert(QStringLiteral("media_id"), id);
                    if (!item.contains(QStringLiteral("media_type")))
                        item.insert(QStringLiteral("media_type"),
                                    item.value(QStringLiteral("content_type")).toString().startsWith(
                                        QStringLiteral("image/")) ? QStringLiteral("image") : QStringLiteral("video"));
                    media.append(item);
                }
                body.remove(QStringLiteral("videos"));
                body.insert(QStringLiteral("media"), media);
                return MaiToolResult::success(
                    QJsonDocument(body).toJson(QJsonDocument::Compact).toStdString());
            }
            if (name == "maichat_cloud_drive_list_pending_uploads") return pendingUploads(directory, transfers);
            if (name == "maichat_cloud_drive_pause_upload") {
                const QString taskId = args.value(QStringLiteral("task_id")).toString();
                const QString checkpoint = QDir(directory).filePath(taskId + QStringLiteral(".maicloud.json"));
                if (transfers->pause(checkpoint))
                    return MaiToolResult::success(QJsonDocument(QJsonObject{
                        {QStringLiteral("accepted"), true}, {QStringLiteral("task_id"), taskId},
                        {QStringLiteral("state"), QStringLiteral("pausing")}})
                        .toJson(QJsonDocument::Compact).toStdString());
                if (transfers->contains(checkpoint))
                    return failure(MaiErrorCode::Busy, "upload task is being discarded");
                if (checkpointByTask(directory, taskId).isEmpty())
                    return failure(MaiErrorCode::NotFound, "upload task was not found");
                return MaiToolResult::success(QJsonDocument(QJsonObject{
                    {QStringLiteral("paused"), true}, {QStringLiteral("task_id"), taskId},
                    {QStringLiteral("state"), QStringLiteral("paused")}})
                    .toJson(QJsonDocument::Compact).toStdString());
            }
            // Avoid hashing a 2 GB file before discovering that the account feature is disabled.
            const MaiToolResult gate = request(operation, MAI_CLOUD_DRIVE_LIST_VIDEOS, {});
            if (gate.hasError()) return gate;
            if (name == "maichat_cloud_drive_upload_media") {
                const QString source = args.value(QStringLiteral("file_path")).toString();
                const QString task = QUuid::createUuid().toString(QUuid::WithoutBraces).toLower();
                const QString checkpoint = QDir(directory).filePath(task + QStringLiteral(".maicloud.json"));
                return queueUpload(access, source, checkpoint, QFileInfo(source).fileName(), transfers);
            }
            if (name == "maichat_cloud_drive_resume_upload") {
                const QJsonObject pending =
                    checkpointByTask(directory, args.value(QStringLiteral("task_id")).toString());
                const QString source = pending.value(QStringLiteral("source_id")).toString();
                const QString checkpoint = pending.value(QStringLiteral("checkpoint_path")).toString();
                const QString display = pending.value(QStringLiteral("name")).toString();
                if (source.isEmpty() || checkpoint.isEmpty() || display.isEmpty())
                    return failure(MaiErrorCode::NotFound, "paused upload task was not found");
                return queueUpload(access, source, checkpoint, display, transfers);
            }
            if (name == "maichat_cloud_drive_abort_upload") {
                const QString taskId = args.value(QStringLiteral("task_id")).toString();
                const QString expectedCheckpoint = QDir(directory).filePath(taskId + QStringLiteral(".maicloud.json"));
                if (transfers->contains(expectedCheckpoint))
                    return failure(MaiErrorCode::Busy, "pause the active upload before discarding it");
                const QJsonObject pending = checkpointByTask(directory, taskId);
                const QString checkpoint = pending.value(QStringLiteral("checkpoint_path")).toString();
                if (checkpoint.isEmpty())
                    return failure(MaiErrorCode::NotFound, "paused upload task was not found");
                TransferRegistration registration(transfers, ActiveTransfer{
                    access.accountId, checkpoint, pending.value(QStringLiteral("name")).toString(),
                    pending.value(QStringLiteral("size_bytes")).toVariant().toLongLong(),
                    operation.cancel, false});
                if (!registration.registered())
                    return failure(MaiErrorCode::Busy, "pause the active upload before discarding it");
                const QByteArray path = checkpoint.toUtf8();
                MaiCloudDriveResult result{};
                const MaiCloudDriveOptions options = operation.options();
                mai_cloud_drive_abort_resumable(&options, path.constData(), operation.cancel.get(), &result);
                const MaiToolResult aborted = resultJson(result);
                mai_cloud_drive_result_free(&result);
                if (aborted.hasError()) return aborted;
                const QJsonObject server = parseObject(aborted.output());
                return MaiToolResult::success(QJsonDocument(QJsonObject{
                    {QStringLiteral("aborted"), true}, {QStringLiteral("task_id"), taskId},
                    {QStringLiteral("cleanup_pending"), server.value(QStringLiteral("cleanup_pending")).toBool()}})
                    .toJson(QJsonDocument::Compact).toStdString());
            }
            const QString mediaId = args.value(QStringLiteral("media_id")).toString();
            if (name == "maichat_cloud_drive_delete_media") {
                const MaiToolResult removed = request(operation, MAI_CLOUD_DRIVE_DELETE_VIDEO,
                    {{QStringLiteral("video_id"), mediaId}});
                if (removed.hasError()) return removed;
                QJsonObject body = parseObject(removed.output());
                body.remove(QStringLiteral("video_id"));
                body.insert(QStringLiteral("media_id"), mediaId);
                body.insert(QStringLiteral("deleted"),
                            body.value(QStringLiteral("status")) == QStringLiteral("deleted"));
                return MaiToolResult::success(
                    QJsonDocument(body).toJson(QJsonDocument::Compact).toStdString());
            }
            if (name == "maichat_cloud_drive_open_media") {
                const MaiToolResult link =
                    request(operation, MAI_CLOUD_DRIVE_PLAY_LINK, {{QStringLiteral("video_id"), mediaId}});
                if (link.hasError()) return link;
                const QJsonObject details = parseObject(link.output());
                const QString signedUrl = details.value(QStringLiteral("read_url")).toString();
                const QString contentType = details.value(QStringLiteral("content_type")).toString();
                const QString suffix = suffixForContentType(contentType);
                const bool image = contentType.startsWith(QStringLiteral("image/"));
                const QString mediaType = details.value(QStringLiteral("media_type")).toString();
                const qint64 size = details.value(QStringLiteral("size_bytes")).toVariant().toLongLong();
                if (!image) {
                    if (!signedUrl.startsWith(QStringLiteral("https://")) || suffix.isEmpty() ||
                        size <= 0 || quint64(size) > kMaxVideoBytes ||
                        mediaType != QStringLiteral("video"))
                        return failure(MaiErrorCode::Protocol, "cloud video link or type is invalid");
                    auto vodCancel = std::shared_ptr<MaiCloudVodCancel>(
                        mai_cloud_vod_cancel_create(), mai_cloud_vod_cancel_destroy);
                    MaiCloudVodResult vodResult{};
                    const DesktopCloudVod vod = desktopOpenCloudVod(
                        access, mediaId, operation.cancel, vodCancel, &vodResult);
                    if (!vod) {
                        return failure(vodResult.status == MAI_CLOUD_VOD_CANCELED ?
                            MaiErrorCode::Canceled : MaiErrorCode::Network,
                            "cloud video-on-demand source could not open");
                    }
                    if (target.isNull() || provider().accountId != access.accountId)
                        return failure(MaiErrorCode::Canceled, "MaiChat account changed before playback");
                    bool opened = false;
                    const QString displayName = QStringLiteral("云盘视频.") + suffix;
                    const auto show = [&] {
                        if (!target.isNull() && open) opened = open(displayName, false, vod);
                    };
                    if (QThread::currentThread() == target->thread()) show();
                    else QMetaObject::invokeMethod(target.data(), show,
                                                   Qt::BlockingQueuedConnection);
                    if (!opened)
                        return failure(MaiErrorCode::Internal, "the video viewer could not open the stream");
                    return MaiToolResult::success(QJsonDocument(QJsonObject{
                        {QStringLiteral("opened"), true}, {QStringLiteral("media_id"), mediaId},
                        {QStringLiteral("media_type"), QStringLiteral("video")}})
                        .toJson(QJsonDocument::Compact).toStdString());
                }
                const QString directory = playbackDirectory();
                if (!signedUrl.startsWith(QStringLiteral("https://")) || directory.isEmpty() ||
                    suffix.isEmpty() || size <= 0 || quint64(size) > (image ? kMaxImageBytes : kMaxVideoBytes) ||
                    mediaType != (image ? QStringLiteral("image") : QStringLiteral("video")))
                    return failure(MaiErrorCode::Protocol, "cloud media link or type is invalid");
                const QString path = QDir(directory).filePath(QUuid::createUuid().toString(QUuid::WithoutBraces) +
                                                              QStringLiteral(".") + suffix);
                const QByteArray url = signedUrl.toUtf8();
                const QByteArray targetPath = path.toUtf8();
                MaiDownloadOptions options{};
                options.url_utf8 = url.constData();
                options.target_path_utf8 = targetPath.constData();
                options.max_bytes = image ? kMaxImageBytes : kMaxVideoBytes;
                options.transfer_timeout_ms = 2 * 60 * 60 * 1000;
                options.ca_pem = operation.roots.isEmpty() ? nullptr : operation.roots.constData();
                options.ca_pem_length = size_t(operation.roots.size());
                options.proxy_url_utf8 = access.proxyUrl.isNull() ? nullptr : operation.proxy.constData();
                MaiDownloadResult downloaded{};
                mai_download_file(&options, operation.downloadCancel.get(), &downloaded);
                if (downloaded.status != MAI_DOWNLOAD_OK) {
                    QFile::remove(path);
                    return failure(
                        downloaded.status == MAI_DOWNLOAD_CANCELED ? MaiErrorCode::Canceled : MaiErrorCode::Network,
                        "cloud media could not be downloaded for preview");
                }
                if (target.isNull() || provider().accountId != access.accountId) {
                    QFile::remove(path);
                    return failure(MaiErrorCode::Canceled, "MaiChat account changed before playback");
                }
                bool opened = false;
                const auto show = [&] {
                    if (!target.isNull() && open) opened = open(path, true, {});
                };
                if (QThread::currentThread() == target->thread())
                    show();
                else
                    QMetaObject::invokeMethod(target.data(), show, Qt::BlockingQueuedConnection);
                if (!opened) {
                    QFile::remove(path);
                    return failure(MaiErrorCode::Internal, "the media viewer could not open the file");
                }
                return MaiToolResult::success(QJsonDocument(QJsonObject{
                    {QStringLiteral("opened"), true}, {QStringLiteral("media_id"), mediaId},
                    {QStringLiteral("media_type"), image ? QStringLiteral("image") : QStringLiteral("video")}})
                    .toJson(QJsonDocument::Compact).toStdString());
            }
            return failure(MaiErrorCode::InvalidInput, "unknown cloud drive operation");
        });
}
