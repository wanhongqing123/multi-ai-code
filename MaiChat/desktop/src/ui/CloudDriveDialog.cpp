#include "ui/CloudDriveDialog.h"

#include "MaiCloudDriveClient.h"
#include "MaiCloudVod.h"
#include "MaiCurlDownload.h"
#if defined(MAICHAT_FFMPEG_IMAGE_DECODE)
#include "MaiImageDecode.h"
#endif
#include "ui/AppMessageDialog.h"
#include "ui/AppTextInputDialog.h"
#include "ui/ImagePreviewDialog.h"
#include "ui/DesktopCloudVod.h"

#include <QCloseEvent>
#include <QColorSpace>
#include <QCryptographicHash>
#include <QDir>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QHash>
#include <QImage>
#include <QImageReader>
#include <QJsonDocument>
#include <QLabel>
#include <QProgressBar>
#include <QPalette>
#include <QPointer>
#include <QPixmap>
#include <QResource>
#include <QPushButton>
#include <QScrollArea>
#include <QSet>
#include <QStandardPaths>
#include <QStyle>
#include <QTemporaryDir>
#include <QTimer>
#include <QUuid>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <atomic>

static QByteArray cloudDriveTrustedRoots(const DesktopVideoCircleAccess& access) {
    if (!access.caPem.isEmpty()) return access.caPem;
    Q_INIT_RESOURCE(AuthCertificates);
    QFile certificates(QStringLiteral(":/maichat/auth/cacert.pem"));
    return certificates.open(QIODevice::ReadOnly) ? certificates.readAll() : QByteArray();
}

namespace {

constexpr quint64 kMaxVideoBytes = 2'000'000'000ULL;
constexpr quint64 kMaxImageBytes = 20'000'000ULL;
constexpr quint64 kMaxThumbnailBytes = 5'000'000ULL;

QString errorText(int status, long http) {
    if (http == 401) return QStringLiteral("登录已过期，请重新登录。");
    if (http == 404 || http == 503) return QStringLiteral("云盘暂不可用，请稍后重试。");
    if (http == 413) return QStringLiteral("文件超出大小限制或 10 GB 账号空间已满。");
    if (http == 429) return QStringLiteral("未完成的上传太多，请稍后重试或放弃旧任务。");
    if (http == 410) return QStringLiteral("上传会话已过期，请重新上传。");
    if (http == 409) return QStringLiteral("操作未完成：可能存在同名文件夹，或文件夹还包含文件。请刷新后重试。");
    if (http == 400) return QStringLiteral("名称或目标位置无效，请检查后重试。");
    if (status == MAI_CLOUD_DRIVE_CHECKPOINT_MISMATCH)
        return QStringLiteral("所选文件与原上传任务不一致，请选择原文件。");
    if (status == MAI_CLOUD_DRIVE_PART_MISMATCH)
        return QStringLiteral("云端分片状态异常，请稍后重试。");
    if (status == MAI_CLOUD_DRIVE_CANCELED) return QStringLiteral("已暂停，可稍后继续。");
    if (status == MAI_CLOUD_DRIVE_FILE_ERROR) return QStringLiteral("文件无法读取，请检查文件位置。");
    if (status == MAI_CLOUD_DRIVE_NETWORK_ERROR) return QStringLiteral("网络连接失败，上传进度已保留。");
    return QStringLiteral("操作失败，请稍后重试。");
}

QString humanSize(quint64 bytes) {
    if (bytes >= 1'000'000'000ULL)
        return QStringLiteral("%1 GB").arg(double(bytes) / 1'000'000'000.0, 0, 'f', 2);
    if (bytes >= 1'000'000ULL)
        return QStringLiteral("%1 MB").arg(double(bytes) / 1'000'000.0, 0, 'f', 1);
    return QStringLiteral("%1 KB").arg(double(bytes) / 1000.0, 0, 'f', 1);
}

QString accountDirectory(const QString& account) {
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (base.isEmpty() || account.isEmpty()) return {};
    const QString key = QString::fromLatin1(QCryptographicHash::hash(
        account.toUtf8(), QCryptographicHash::Sha256).toHex());
    const QString dir = QDir(base).filePath(QStringLiteral("CloudDrive/") + key);
    return QDir().mkpath(dir) ? dir : QString();
}

void clearRows(QVBoxLayout* rows) {
    while (QLayoutItem* item = rows->takeAt(0)) {
        if (item->widget()) {
            item->widget()->hide();
            item->widget()->deleteLater();
        }
        delete item;
    }
}

QScrollArea* makeScroll(QVBoxLayout** rows, QWidget* parent) {
    auto* scroll = new QScrollArea(parent);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setStyleSheet(QStringLiteral("QScrollArea{background:#fff;border:0;}"));
    auto* content = new QWidget(scroll);
    QPalette surface = content->palette();
    surface.setColor(QPalette::Window, Qt::white);
    content->setPalette(surface);
    content->setAutoFillBackground(true);
    *rows = new QVBoxLayout(content);
    (*rows)->setContentsMargins(0, 0, 0, 0);
    (*rows)->setSpacing(8);
    (*rows)->setAlignment(Qt::AlignTop);
    scroll->setWidget(content);
    return scroll;
}

struct ProgressState {
    std::atomic<quint64> uploaded{0};
    std::atomic<quint64> total{0};
};

int uploadProgress(uint64_t uploaded, uint64_t total, void* context) {
    auto* state = static_cast<ProgressState*>(context);
    state->uploaded.store(uploaded, std::memory_order_relaxed);
    state->total.store(total, std::memory_order_relaxed);
    return 0;
}

QImage decodeCloudImage(const QString& path, int maxWidth, int maxHeight) {
#if defined(MAICHAT_FFMPEG_IMAGE_DECODE)
    const QByteArray utf8 = path.toUtf8();
    const MaiImageDecodeResult decoded = maiImageDecodeFile(
        utf8.constData(), maxWidth, maxHeight);
    QImage image;
    if (decoded.rgba) {
        image = QImage(decoded.rgba, decoded.width, decoded.height, decoded.stride,
                       QImage::Format_RGBA8888).copy();
        if (decoded.icc_profile && decoded.icc_size > 0) {
            const QColorSpace color = QColorSpace::fromIccProfile(
                QByteArray(reinterpret_cast<const char*>(decoded.icc_profile),
                           decoded.icc_size));
            if (color.isValid()) image.setColorSpace(color);
        }
        maiImageDecodeFree(decoded.rgba);
    }
    return image;
#else
    QImageReader reader(path);
    reader.setAutoTransform(true);
    QSize scaled = reader.size();
    if (scaled.isValid()) {
        scaled.scale(maxWidth, maxHeight, Qt::KeepAspectRatio);
        reader.setScaledSize(scaled);
    }
    return reader.read();
#endif
}

MaiDownloadResult downloadCloudFile(const DesktopVideoCircleAccess& access,
                                    const QString& url, const QString& path,
                                    quint64 maxBytes, int timeoutMs,
                                    const MaiDownloadCancel* cancel) {
    const QByteArray address = url.toUtf8();
    const QByteArray target = path.toUtf8();
    const QByteArray proxy = access.proxyUrl.toUtf8();
    const QByteArray roots = cloudDriveTrustedRoots(access);
    MaiDownloadOptions options{};
    options.url_utf8 = address.constData();
    options.target_path_utf8 = target.constData();
    options.max_bytes = maxBytes;
    options.transfer_timeout_ms = timeoutMs;
    options.ca_pem = roots.isEmpty() ? nullptr : roots.constData();
    options.ca_pem_length = size_t(roots.size());
    options.proxy_url_utf8 = access.proxyUrl.isNull() ? nullptr : proxy.constData();
    MaiDownloadResult result{};
    mai_download_file(&options, cancel, &result);
    return result;
}

} // namespace

struct CloudDriveDialog::Reply {
    int status = MAI_CLOUD_DRIVE_INVALID_ARGUMENT;
    long http = 0;
    QJsonObject body;
    QString error;
    QString notice;
    bool uploadedInRoot = false;
    DesktopCloudVod vod;
    bool ok() const { return status == MAI_CLOUD_DRIVE_OK && http >= 200 && http < 300; }
};

struct CloudDriveDialog::ImageReply {
    Reply reply;
    QImage image;
};

CloudDriveDialog::Reply CloudDriveDialog::request(const DesktopVideoCircleAccess& access,
    int action, const QJsonObject& payload, const MaiCloudDriveCancel* cancel) {
    Reply reply;
    if (!access.isValid()) { reply.error = QStringLiteral("请先登录 MaiChat。"); return reply; }
    const QByteArray base = access.authBaseUrl.toUtf8();
    const QByteArray token = access.accountToken.toUtf8();
    const QByteArray proxy = access.proxyUrl.toUtf8();
    const QByteArray host = access.signedUploadHostSuffix.toUtf8();
    const QByteArray roots = cloudDriveTrustedRoots(access);
    const QByteArray json = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    MaiCloudDriveOptions options{};
    options.auth_base_url_utf8 = base.constData();
    options.account_token_utf8 = token.constData();
    options.ca_pem = roots.isEmpty() ? nullptr : roots.constData();
    options.ca_pem_length = size_t(roots.size());
    options.proxy_url_utf8 = access.proxyUrl.isNull() ? nullptr : proxy.constData();
    options.signed_upload_host_suffix_utf8 = host.isEmpty() ? nullptr : host.constData();
    MaiCloudDriveResult result{};
    mai_cloud_drive_request(&options, MaiCloudDriveAction(action), json.constData(), cancel, &result);
    reply.status = result.status;
    reply.http = result.http_status;
    if (result.response_json_utf8)
        reply.body = QJsonDocument::fromJson(QByteArray(result.response_json_utf8,
            int(result.response_bytes))).object();
    if (!reply.ok()) reply.error = errorText(reply.status, reply.http);
    mai_cloud_drive_result_free(&result);
    return reply;
}

int CloudDriveDialog::availability(const DesktopVideoCircleAccess& access) {
    const Reply reply = request(access, MAI_CLOUD_DRIVE_LIST_VIDEOS, {}, nullptr);
    if (reply.ok()) return 1;
    if (reply.http == 404 || reply.http == 503) return 0;
    return -1;
}

CloudDriveDialog::CloudDriveDialog(AccessProvider access, AccountIdProvider accountId,
    PlayCallback play, QWidget* parent, RelocateSelector relocate, bool embedded)
    : QWidget(parent), accessProvider_(std::move(access)),
      accountIdProvider_(std::move(accountId)), play_(std::move(play)),
      relocate_(std::move(relocate)), embedded_(embedded) {
    setObjectName(QStringLiteral("cloudDriveDialog"));
    if (embedded_) setWindowFlags(Qt::Widget);
    setWindowTitle(QStringLiteral("我的云盘"));
    resize(740, 640);
    setMinimumSize(540, 450);
    setStyleSheet(QStringLiteral(
        "QWidget#cloudDriveDialog{background:#fff;color:#172033;}"
        "QPushButton{padding:7px 12px;border:0;border-radius:8px;background:#eef6fd;color:#0b67b7;}"
        "QPushButton:hover{background:#dceefb;}"
        "QPushButton:disabled{background:#f4f5f7;color:#98a2b3;}"
        "QProgressBar{border:0;border-radius:5px;background:#e9eef4;height:10px;}"
        "QProgressBar::chunk{border-radius:5px;background:#1389d2;}"));
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(20, 18, 20, 18);
    root->setSpacing(12);
    auto* header = new QHBoxLayout;
    auto* heading = new QLabel(QStringLiteral("我的云盘"), this);
    heading->setStyleSheet(QStringLiteral("font-size:21px;font-weight:700;"));
    header->addWidget(heading);
    header->addStretch();
    newFolderButton_ = new QPushButton(QStringLiteral("新建文件夹"), this);
    newFolderButton_->setObjectName(QStringLiteral("cloudDriveNewFolder"));
    connect(newFolderButton_, &QPushButton::clicked, this, &CloudDriveDialog::createFolder);
    header->addWidget(newFolderButton_);
    uploadButton_ = new QPushButton(QStringLiteral("上传图片或视频"), this);
    uploadButton_->setObjectName(QStringLiteral("cloudDriveUpload"));
    connect(uploadButton_, &QPushButton::clicked, this, [this] {
        const QString file = QFileDialog::getOpenFileName(this, QStringLiteral("选择图片或视频"), {},
            QStringLiteral("图片和视频 (*.jpg *.jpeg *.png *.webp *.heic *.mp4 *.mov)"));
        if (!file.isEmpty()) startUpload(file);
    });
    header->addWidget(uploadButton_);
    auto* close = new QPushButton(QStringLiteral("关闭"), this);
    connect(close, &QPushButton::clicked, this, &QWidget::close);
    if (embedded_) close->hide();
    else header->addWidget(close);
    root->addLayout(header);

    auto* quotaCard = new QFrame(this);
    quotaCard->setObjectName(QStringLiteral("cloudDriveQuotaCard"));
    quotaCard->setStyleSheet(QStringLiteral(
        "QFrame#cloudDriveQuotaCard{background:#f7fbfe;border:1px solid #e1edf6;"
        "border-radius:12px;}"));
    auto* quotaLayout = new QVBoxLayout(quotaCard);
    quotaLayout->setContentsMargins(16, 14, 16, 14);
    quotaLayout->setSpacing(9);
    auto* quotaHeading = new QHBoxLayout;
    auto* quotaTitle = new QLabel(QStringLiteral("我的空间"), quotaCard);
    quotaTitle->setStyleSheet(QStringLiteral("color:#172033;font-weight:650;"));
    quotaHeading->addWidget(quotaTitle);
    quotaHeading->addStretch();
    quota_ = new QLabel(QStringLiteral("正在读取空间…"), quotaCard);
    quota_->setObjectName(QStringLiteral("cloudDriveQuota"));
    quota_->setStyleSheet(QStringLiteral("color:#344054;font-weight:600;"));
    quotaHeading->addWidget(quota_);
    quotaLayout->addLayout(quotaHeading);
    quotaBar_ = new QProgressBar(quotaCard);
    quotaBar_->setObjectName(QStringLiteral("cloudDriveQuotaBar"));
    quotaBar_->setRange(0, 1000);
    quotaBar_->setValue(0);
    quotaBar_->setTextVisible(false);
    quotaBar_->setFixedHeight(8);
    quotaLayout->addWidget(quotaBar_);
    quotaCaption_ = new QLabel(QStringLiteral("仅自己可见 · 视频最多 2 GB · 图片最多 20 MB"), quotaCard);
    quotaCaption_->setStyleSheet(QStringLiteral("color:#667085;font-size:12px;"));
    quotaLayout->addWidget(quotaCaption_);
    root->addWidget(quotaCard);
    uploadScroll_ = makeScroll(&uploadRows_, this);
    // 待续传仅在有真实任务时占位；空区域不再把文件列表向下推，也无需重复分区标题。
    uploadScroll_->hide();
    root->addWidget(uploadScroll_);
    auto* navigation = new QHBoxLayout;
    navigation->setSpacing(6);
    rootFolderButton_ = new QPushButton(QStringLiteral("根目录"), this);
    rootFolderButton_->setObjectName(QStringLiteral("cloudDriveRootFolder"));
    connect(rootFolderButton_, &QPushButton::clicked, this, [this] { navigateToFolder({}); });
    navigation->addWidget(rootFolderButton_);
    parentFolderButton_ = new QPushButton(QStringLiteral("上一级"), this);
    parentFolderButton_->setObjectName(QStringLiteral("cloudDriveParentFolder"));
    connect(parentFolderButton_, &QPushButton::clicked, this, [this] {
        for (const auto& value : folders_) {
            const QJsonObject folder = value.toObject();
            if (folder.value(QStringLiteral("id")).toString() == currentFolderId_) {
                navigateToFolder(folder.value(QStringLiteral("parent_id")).toString());
                return;
            }
        }
        navigateToFolder({});
    });
    navigation->addWidget(parentFolderButton_);
    folderPath_ = new QLabel(QStringLiteral("我的云盘"), this);
    folderPath_->setObjectName(QStringLiteral("cloudDriveFolderPath"));
    folderPath_->setWordWrap(true);
    folderPath_->setMinimumWidth(0);
    navigation->addWidget(folderPath_, 1);
    refreshButton_ = new QPushButton(QStringLiteral("刷新"), this);
    refreshButton_->setObjectName(QStringLiteral("cloudDriveRefresh"));
    connect(refreshButton_, &QPushButton::clicked, this, [this] { refresh(); });
    navigation->addWidget(refreshButton_);
    root->addLayout(navigation);
    root->addWidget(makeScroll(&videoRows_, this), 1);
    moreButton_ = new QPushButton(QStringLiteral("加载更多"), this);
    moreButton_->setObjectName(QStringLiteral("cloudDriveMore"));
    connect(moreButton_, &QPushButton::clicked, this, [this] { refresh(true); });
    root->addWidget(moreButton_, 0, Qt::AlignHCenter);
    progress_ = new QProgressBar(this);
    progress_->setObjectName(QStringLiteral("cloudDriveProgress"));
    progress_->setRange(0, 100);
    progress_->hide();
    root->addWidget(progress_);
    auto* statusRow = new QHBoxLayout;
    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("cloudDriveStatus"));
    status_->setWordWrap(true);
    statusRow->addWidget(status_, 1);
    pauseButton_ = new QPushButton(QStringLiteral("暂停"), this);
    pauseButton_->setObjectName(QStringLiteral("cloudDrivePause"));
    pauseButton_->hide();
    connect(pauseButton_, &QPushButton::clicked, this, &CloudDriveDialog::cancelPending);
    statusRow->addWidget(pauseButton_);
    root->addLayout(statusRow);
}

CloudDriveDialog::~CloudDriveDialog() { cancelPending(); }

bool CloudDriveDialog::sameAccount() const {
    return !openedAccountId_.isEmpty() && accountIdProvider_ && accessProvider_ &&
           accountIdProvider_() == openedAccountId_ &&
           accessProvider_().accountId == openedAccountId_;
}

void CloudDriveDialog::openDrive(const QString& suggestedVideo) {
    const DesktopVideoCircleAccess access = accessProvider_ ? accessProvider_() :
        DesktopVideoCircleAccess{};
    const QString currentAccount = accountIdProvider_ ? accountIdProvider_() : QString();
    if (!access.isValid() || currentAccount.isEmpty() || access.accountId != currentAccount) {
        cancelPending();
        resetThumbnails();
        ++generation_;
        requestCancel_.reset();
        downloadCancel_.reset();
        vodCancel_.reset();
        openedAccountId_.clear();
        nextCursor_.clear();
        currentFolderId_.clear();
        folders_ = {};
        refreshNotice_.clear();
        updateFolderNavigation();
        clearRows(videoRows_);
        renderCheckpoints({});
        setBusy(false);
        quota_->setText(QStringLiteral("未连接"));
        quotaBar_->setValue(0);
        setStatus(QStringLiteral("请先登录 MaiChat。"), true);
        show();
        return;
    }
    if (openedAccountId_ != currentAccount) {
        cancelPending();
        resetThumbnails();
        ++generation_;
        requestCancel_.reset();
        downloadCancel_.reset();
        setBusy(false);
        openedAccountId_ = currentAccount;
        nextCursor_.clear();
        currentFolderId_.clear();
        folders_ = {};
        refreshNotice_.clear();
        updateFolderNavigation();
        quota_->setText(QStringLiteral("正在读取空间…"));
        quotaBar_->setValue(0);
        clearRows(videoRows_);
        renderCheckpoints({});
    }
    show();
    if (!embedded_) {
        raise();
        activateWindow();
    }
    refreshCheckpoints();
    if (suggestedVideo.isEmpty()) refresh();
    else {
        auto* retry = new QTimer(this);
        retry->setInterval(50);
        connect(retry, &QTimer::timeout, this, [this, retry, suggestedVideo, currentAccount] {
            if (!isVisible() || openedAccountId_ != currentAccount) {
                retry->stop(); retry->deleteLater(); return;
            }
            if (busy_) return;
            retry->stop(); retry->deleteLater();
            startUpload(suggestedVideo);
        });
        retry->start();
    }
}

void CloudDriveDialog::cancelPending() {
    if (requestCancel_) mai_cloud_drive_cancel_request(requestCancel_.get());
    if (downloadCancel_) mai_download_cancel_request(downloadCancel_.get());
    if (vodCancel_) mai_cloud_vod_cancel_request(vodCancel_.get());
    if (thumbnailRequestCancel_)
        mai_cloud_drive_cancel_request(thumbnailRequestCancel_.get());
    if (thumbnailDownloadCancel_)
        mai_download_cancel_request(thumbnailDownloadCancel_.get());
    if (busy_) setStatus(vodCancel_ ? QStringLiteral("正在停止点播…") :
                         QStringLiteral("正在暂停，已确认的分片会保留…"));
}

void CloudDriveDialog::closeEvent(QCloseEvent* event) {
    cancelPending();
    QWidget::closeEvent(event);
}

void CloudDriveDialog::setStatus(const QString& message, bool error) {
    status_->setText(message);
    status_->setStyleSheet(error ? QStringLiteral("color:#c62b36;")
                                 : QStringLiteral("color:#667085;"));
}

void CloudDriveDialog::setBusy(bool busy, const QString& message) {
    busy_ = busy;
    uploadButton_->setEnabled(!busy);
    newFolderButton_->setEnabled(!busy && sameAccount());
    refreshButton_->setEnabled(!busy && sameAccount());
    rootFolderButton_->setEnabled(!busy && !currentFolderId_.isEmpty());
    parentFolderButton_->setEnabled(!busy && !currentFolderId_.isEmpty());
    moreButton_->setEnabled(!busy && !nextCursor_.isEmpty());
    for (const QString& name : {QStringLiteral("cloudDriveResume"),
                                QStringLiteral("cloudDriveDiscard"),
                                QStringLiteral("cloudDrivePlay"),
                                QStringLiteral("cloudDriveViewImage"),
                                QStringLiteral("cloudDriveDelete"),
                                QStringLiteral("cloudDriveRenameMedia"),
                                QStringLiteral("cloudDriveMoveMedia"),
                                QStringLiteral("cloudDriveFolderOpen"),
                                QStringLiteral("cloudDriveFolderRename"),
                                QStringLiteral("cloudDriveFolderMove"),
                                QStringLiteral("cloudDriveFolderDelete")}) {
        for (QPushButton* action : findChildren<QPushButton*>(name))
            action->setEnabled(!busy);
    }
    pauseButton_->setVisible(busy);
    progress_->setVisible(busy);
    if (!busy) progress_->setRange(0, 100);
    if (!message.isEmpty()) setStatus(message);
}

void CloudDriveDialog::refreshCheckpoints() {
    if (!sameAccount()) return;
    const QString directory = accountDirectory(openedAccountId_);
    if (directory.isEmpty()) { setStatus(QStringLiteral("无法建立云盘任务目录。"), true); return; }
    const QByteArray path = directory.toUtf8();
    MaiCloudDriveResult result{};
    mai_cloud_drive_list_local_checkpoints(path.constData(), &result);
    if (result.status == MAI_CLOUD_DRIVE_OK && result.response_json_utf8) {
        const QJsonObject body = QJsonDocument::fromJson(QByteArray(
            result.response_json_utf8, int(result.response_bytes))).object();
        renderCheckpoints(body.value(QStringLiteral("uploads")).toArray());
    }
    mai_cloud_drive_result_free(&result);
}

void CloudDriveDialog::renderCheckpoints(const QJsonArray& uploads) {
    clearRows(uploadRows_);
    if (uploadScroll_) {
        uploadScroll_->setVisible(!uploads.isEmpty());
        if (!uploads.isEmpty()) uploadScroll_->setFixedHeight(qMin(170, 18 + uploads.size() * 70));
    }
    // 只收起展示，不删除检查点；非空仍复用下方继续/放弃动作和原上传进度实现。
    if (uploads.isEmpty()) return;
    for (const QJsonValue& value : uploads) {
        const QJsonObject task = value.toObject();
        const QString checkpoint = task.value(QStringLiteral("checkpoint_path")).toString();
        const QString source = task.value(QStringLiteral("source_id")).toString();
        const QString name = task.value(QStringLiteral("name")).toString();
        auto* row = new QFrame(this);
        row->setMaximumHeight(64);
        row->setStyleSheet(QStringLiteral("QFrame{background:#fff;border:1px solid #e6ebf1;border-radius:9px;}"
                                          "QLabel{border:0;}QPushButton{border:0;}"));
        auto* layout = new QHBoxLayout(row);
        auto* label = new QLabel(name + QStringLiteral(" · ") +
            humanSize(quint64(task.value(QStringLiteral("size_bytes")).toDouble())), row);
        label->setToolTip(source);
        layout->addWidget(label, 1);
        auto* resume = new QPushButton(QStringLiteral("继续"), row);
        resume->setObjectName(QStringLiteral("cloudDriveResume"));
        connect(resume, &QPushButton::clicked, this, [this, source, checkpoint, name] {
            QString path = source;
            if (!QFileInfo::exists(path)) {
                path = relocate_ ? relocate_(name) :
                    QFileDialog::getOpenFileName(this,
                        QStringLiteral("原文件已移动，请重新选择"), {},
                        QStringLiteral("图片和视频 (*.jpg *.jpeg *.png *.webp *.heic *.mp4 *.mov)"));
            }
            if (!path.isEmpty()) startUpload(path, checkpoint, name);
        });
        layout->addWidget(resume);
        auto* discard = new QPushButton(QStringLiteral("放弃"), row);
        discard->setObjectName(QStringLiteral("cloudDriveDiscard"));
        connect(discard, &QPushButton::clicked, this,
                [this, checkpoint, name] { discardUpload(checkpoint, name); });
        layout->addWidget(discard);
        uploadRows_->addWidget(row);
    }
}

void CloudDriveDialog::refresh(bool append) {
    if (busy_ || !sameAccount()) return;
    if (append && nextCursor_.isEmpty()) return;
    if (!append) resetThumbnails();
    const QString account = openedAccountId_;
    const QString cursor = append ? nextCursor_ : QString();
    const QString folderId = currentFolderId_;
    const DesktopVideoCircleAccess access = accessProvider_();
    const auto cancel = std::shared_ptr<MaiCloudDriveCancel>(
        mai_cloud_drive_cancel_create(), mai_cloud_drive_cancel_destroy);
    requestCancel_ = cancel;
    const unsigned generation = ++generation_;
    setBusy(true, QStringLiteral("正在读取云盘…"));
    auto* watcher = new QFutureWatcher<Reply>(this);
    connect(watcher, &QFutureWatcher<Reply>::finished, this,
            [this, watcher, generation, account, append] {
        const Reply reply = watcher->result();
        watcher->deleteLater();
        if (generation != generation_) return;
        requestCancel_.reset();
        setBusy(false);
        if (!sameAccount() || openedAccountId_ != account) return;
        if (!reply.ok()) { setStatus(reply.error, true); return; }
        if (!append) {
            folders_ = reply.body.value(QStringLiteral("folders")).toArray();
            currentFolderId_ = reply.body.value(QStringLiteral("listed_folder_id")).toString();
            updateFolderNavigation();
        }
        const QJsonObject quota = reply.body.value(QStringLiteral("quota")).toObject();
        const quint64 used = quint64(quota.value(QStringLiteral("used_bytes")).toDouble());
        const quint64 reserved = quint64(quota.value(QStringLiteral("reserved_bytes")).toDouble());
        const quint64 limit = quint64(quota.value(QStringLiteral("limit_bytes")).toDouble());
        quota_->setText(QStringLiteral("已使用 %1 / %2")
            .arg(humanSize(used), humanSize(limit)));
        quotaCaption_->setText(reserved ?
            QStringLiteral("上传中预留 %1").arg(humanSize(reserved)) :
            QStringLiteral("仅自己可见 · 视频最多 2 GB · 图片最多 20 MB"));
        const quint64 occupied = qMin(limit, used + reserved);
        quotaBar_->setValue(limit == 0 ? 0 :
            occupied == 0 ? 0 : qMax(1, int((occupied * 1000) / limit)));
        nextCursor_ = reply.body.value(QStringLiteral("next_cursor")).toString();
        renderVideos(reply.body.value(QStringLiteral("videos")).toArray(), append);
        moreButton_->setVisible(!nextCursor_.isEmpty());
        moreButton_->setEnabled(!nextCursor_.isEmpty());
        setStatus(refreshNotice_, !refreshNotice_.isEmpty());
        refreshNotice_.clear();
    });
    watcher->setFuture(QtConcurrent::run([access, cursor, folderId, append, cancel] {
        QJsonArray folders;
        QString listedFolder = folderId;
        if (!append) {
            const Reply tree = request(access, MAI_CLOUD_DRIVE_LIST_FOLDERS, {}, cancel.get());
            if (!tree.ok()) return tree;
            folders = tree.body.value(QStringLiteral("folders")).toArray();
            bool exists = listedFolder.isEmpty();
            for (const auto& value : folders)
                exists |= value.toObject().value(QStringLiteral("id")).toString() == listedFolder;
            if (!exists) listedFolder.clear();
        }
        QJsonObject payload{{QStringLiteral("folder_id"), listedFolder}};
        if (!cursor.isEmpty()) payload.insert(QStringLiteral("before"), cursor);
        Reply reply = request(access, MAI_CLOUD_DRIVE_LIST_VIDEOS, payload, cancel.get());
        reply.body.insert(QStringLiteral("folders"), folders);
        reply.body.insert(QStringLiteral("listed_folder_id"), listedFolder);
        return reply;
    }));
}

QString CloudDriveDialog::folderPath(const QString& folderId) const {
    QStringList names;
    QString current = folderId;
    QSet<QString> visited;
    while (!current.isEmpty() && !visited.contains(current)) {
        visited.insert(current);
        QJsonObject match;
        for (const auto& value : folders_) {
            const QJsonObject folder = value.toObject();
            if (folder.value(QStringLiteral("id")).toString() == current) {
                match = folder;
                break;
            }
        }
        if (match.isEmpty()) break;
        names.prepend(match.value(QStringLiteral("name")).toString());
        current = match.value(QStringLiteral("parent_id")).toString();
    }
    names.prepend(QStringLiteral("我的云盘"));
    return names.join(QStringLiteral(" / "));
}

void CloudDriveDialog::updateFolderNavigation() {
    folderPath_->setText(folderPath(currentFolderId_));
    folderPath_->setToolTip(folderPath_->text());
    rootFolderButton_->setEnabled(!busy_ && !currentFolderId_.isEmpty());
    parentFolderButton_->setEnabled(!busy_ && !currentFolderId_.isEmpty());
}

void CloudDriveDialog::navigateToFolder(const QString& folderId) {
    if (busy_ || !sameAccount()) return;
    currentFolderId_ = folderId;
    refreshNotice_.clear();
    nextCursor_.clear();
    clearRows(videoRows_);
    updateFolderNavigation();
    refresh();
}

void CloudDriveDialog::createFolder() {
    if (busy_ || !sameAccount()) return;
    AppTextInputDialog::Options options;
    options.title = QStringLiteral("新建文件夹");
    options.description = QStringLiteral("创建位置：%1").arg(folderPath(currentFolderId_));
    options.placeholder = QStringLiteral("文件夹名称");
    options.confirmText = QStringLiteral("创建");
    bool accepted = false;
    const QString name = AppTextInputDialog::getText(this, options, &accepted).trimmed();
    if (!accepted) return;
    if (name.isEmpty()) { setStatus(QStringLiteral("请输入文件夹名称。"), true); return; }
    mutateFolder(MAI_CLOUD_DRIVE_CREATE_FOLDER,
        {{QStringLiteral("name"), name}, {QStringLiteral("parent_id"), currentFolderId_}});
}

void CloudDriveDialog::renameFolder(const QString& folderId, const QString& name) {
    if (busy_ || !sameAccount()) return;
    AppTextInputDialog::Options options;
    options.title = QStringLiteral("重命名文件夹");
    options.initialText = name;
    options.placeholder = QStringLiteral("文件夹名称");
    bool accepted = false;
    const QString renamed = AppTextInputDialog::getText(this, options, &accepted).trimmed();
    if (!accepted || renamed == name) return;
    if (renamed.isEmpty()) { setStatus(QStringLiteral("请输入文件夹名称。"), true); return; }
    mutateFolder(MAI_CLOUD_DRIVE_UPDATE_FOLDER,
        {{QStringLiteral("folder_id"), folderId}, {QStringLiteral("name"), renamed}});
}

void CloudDriveDialog::deleteFolder(const QString& folderId, const QString& name) {
    if (busy_ || !sameAccount()) return;
    if (!AppMessageDialog::confirm(this, QStringLiteral("删除文件夹"),
        QStringLiteral("删除“%1”？只能删除空文件夹，其中的文件不会被删除。")
            .arg(name), QStringLiteral("删除"), true)) return;
    mutateFolder(MAI_CLOUD_DRIVE_DELETE_FOLDER, {{QStringLiteral("folder_id"), folderId}});
}

void CloudDriveDialog::moveItem(const QString& itemId, const QString& name, bool folder) {
    if (busy_ || !sameAccount()) return;
    QDialog selection(this);
    selection.setObjectName(QStringLiteral("cloudDriveMoveDialog"));
    selection.setWindowTitle(QStringLiteral("移动到文件夹"));
    selection.resize(470, 430);
    auto* layout = new QVBoxLayout(&selection);
    auto* prompt = new QLabel(QStringLiteral("将“%1”移动到：").arg(name), &selection);
    prompt->setWordWrap(true);
    layout->addWidget(prompt);
    auto* tree = new QTreeWidget(&selection);
    tree->setObjectName(QStringLiteral("cloudDriveMoveTree"));
    tree->setHeaderHidden(true);
    auto* root = new QTreeWidgetItem(tree, {QStringLiteral("我的云盘")});
    root->setData(0, Qt::UserRole, QString());
    root->setIcon(0, style()->standardIcon(QStyle::SP_DirIcon));
    QHash<QString, QTreeWidgetItem*> items;
    items.insert(QString(), root);
    QJsonArray remaining = folders_;
    while (!remaining.isEmpty()) {
        QJsonArray next;
        bool added = false;
        for (const auto& value : remaining) {
            const QJsonObject entry = value.toObject();
            const QString id = entry.value(QStringLiteral("id")).toString();
            const QString parentId = entry.value(QStringLiteral("parent_id")).toString();
            if (folder && id == itemId) continue;
            if (!items.contains(parentId)) { next.append(value); continue; }
            auto* item = new QTreeWidgetItem(items.value(parentId),
                {entry.value(QStringLiteral("name")).toString()});
            item->setData(0, Qt::UserRole, id);
            item->setIcon(0, style()->standardIcon(QStyle::SP_DirIcon));
            items.insert(id, item);
            added = true;
        }
        if (!added) break; // Excludes descendants of the folder being moved.
        remaining = next;
    }
    tree->expandAll();
    tree->setCurrentItem(items.value(currentFolderId_, root));
    layout->addWidget(tree, 1);
    auto* buttons = new QDialogButtonBox(&selection);
    auto* cancel = buttons->addButton(QStringLiteral("取消"), QDialogButtonBox::RejectRole);
    cancel->setObjectName(QStringLiteral("cloudDriveMoveCancel"));
    auto* move = buttons->addButton(QStringLiteral("移动"), QDialogButtonBox::AcceptRole);
    move->setObjectName(QStringLiteral("cloudDriveMoveConfirm"));
    connect(buttons, &QDialogButtonBox::accepted, &selection, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &selection, &QDialog::reject);
    layout->addWidget(buttons);
    if (selection.exec() != QDialog::Accepted || !tree->currentItem()) return;
    const QString destination = tree->currentItem()->data(0, Qt::UserRole).toString();
    if (destination == currentFolderId_) return;
    mutateFolder(folder ? MAI_CLOUD_DRIVE_UPDATE_FOLDER : MAI_CLOUD_DRIVE_MOVE_MEDIA,
        {{folder ? QStringLiteral("folder_id") : QStringLiteral("media_id"), itemId},
         {folder ? QStringLiteral("parent_id") : QStringLiteral("folder_id"), destination}});
}

void CloudDriveDialog::mutateFolder(int action, const QJsonObject& payload) {
    if (busy_ || !sameAccount()) return;
    const DesktopVideoCircleAccess access = accessProvider_();
    const auto cancel = std::shared_ptr<MaiCloudDriveCancel>(
        mai_cloud_drive_cancel_create(), mai_cloud_drive_cancel_destroy);
    requestCancel_ = cancel;
    const unsigned generation = ++generation_;
    setBusy(true, QStringLiteral("正在更新云盘…"));
    auto* watcher = new QFutureWatcher<Reply>(this);
    connect(watcher, &QFutureWatcher<Reply>::finished, this, [this, watcher, generation] {
        const Reply reply = watcher->result();
        watcher->deleteLater();
        if (generation != generation_) return;
        requestCancel_.reset();
        setBusy(false);
        if (!sameAccount()) return;
        if (!reply.ok()) { setStatus(reply.error, true); return; }
        refresh();
    });
    watcher->setFuture(QtConcurrent::run([access, action, payload, cancel] {
        return request(access, action, payload, cancel.get());
    }));
}

void CloudDriveDialog::renderFolders() {
    for (const auto& value : folders_) {
        const QJsonObject folder = value.toObject();
        if (folder.value(QStringLiteral("parent_id")).toString() != currentFolderId_) continue;
        const QString id = folder.value(QStringLiteral("id")).toString();
        const QString name = folder.value(QStringLiteral("name")).toString();
        auto* row = new QFrame(this);
        row->setObjectName(QStringLiteral("cloudDriveFolderRow"));
        row->setProperty("folderId", id);
        row->setMinimumHeight(64);
        row->setMaximumHeight(72);
        row->setStyleSheet(QStringLiteral("QFrame{background:#f7fbfe;border:1px solid #e6ebf1;"
            "border-radius:9px;}QPushButton{border:0;}"));
        auto* layout = new QHBoxLayout(row);
        auto* open = new QPushButton(name, row);
        open->setObjectName(QStringLiteral("cloudDriveFolderOpen"));
        open->setIcon(style()->standardIcon(QStyle::SP_DirIcon));
        open->setToolTip(name);
        open->setMinimumWidth(0);
        open->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        open->setStyleSheet(QStringLiteral("text-align:left;font-weight:600;background:transparent;"));
        connect(open, &QPushButton::clicked, this, [this, id] { navigateToFolder(id); });
        layout->addWidget(open, 1);
        auto* rename = new QPushButton(QStringLiteral("改名"), row);
        rename->setObjectName(QStringLiteral("cloudDriveFolderRename"));
        connect(rename, &QPushButton::clicked, this, [this, id, name] { renameFolder(id, name); });
        layout->addWidget(rename);
        auto* move = new QPushButton(QStringLiteral("移动"), row);
        move->setObjectName(QStringLiteral("cloudDriveFolderMove"));
        connect(move, &QPushButton::clicked, this, [this, id, name] { moveItem(id, name, true); });
        layout->addWidget(move);
        auto* remove = new QPushButton(QStringLiteral("删除"), row);
        remove->setObjectName(QStringLiteral("cloudDriveFolderDelete"));
        connect(remove, &QPushButton::clicked, this, [this, id, name] { deleteFolder(id, name); });
        layout->addWidget(remove);
        videoRows_->addWidget(row);
    }
}

void CloudDriveDialog::resetThumbnails() {
    if (thumbnailRequestCancel_)
        mai_cloud_drive_cancel_request(thumbnailRequestCancel_.get());
    if (thumbnailDownloadCancel_)
        mai_download_cancel_request(thumbnailDownloadCancel_.get());
    ++thumbnailGeneration_;
    thumbnailRequestCancel_.reset(mai_cloud_drive_cancel_create(),
                                  mai_cloud_drive_cancel_destroy);
    thumbnailDownloadCancel_.reset(mai_download_cancel_create(),
                                   mai_download_cancel_destroy);
    const QString cache = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    thumbnailDirectory_.reset();
    if (!cache.isEmpty() && QDir().mkpath(cache)) {
        auto temporary = std::make_shared<QTemporaryDir>(
            QDir(cache).filePath(QStringLiteral("CloudDriveThumbnails-XXXXXX")));
        if (temporary->isValid()) thumbnailDirectory_ = std::move(temporary);
    }
}

void CloudDriveDialog::loadThumbnail(const QString& mediaId, QLabel* target) {
    if (!sameAccount() || !thumbnailDirectory_ || !thumbnailRequestCancel_ ||
        !thumbnailDownloadCancel_ || !target) return;
    const DesktopVideoCircleAccess access = accessProvider_();
    const auto directory = thumbnailDirectory_;
    const auto requestCancel = thumbnailRequestCancel_;
    const auto downloadCancel = thumbnailDownloadCancel_;
    const unsigned generation = thumbnailGeneration_;
    const QString account = openedAccountId_;
    const QPointer<QLabel> guardedTarget(target);
    auto* watcher = new QFutureWatcher<QImage>(this);
    connect(watcher, &QFutureWatcher<QImage>::finished, this,
            [this, watcher, guardedTarget, generation, account] {
        const QImage image = watcher->result();
        watcher->deleteLater();
        if (image.isNull() || generation != thumbnailGeneration_ ||
            !sameAccount() || openedAccountId_ != account || !guardedTarget) return;
        guardedTarget->setText(QString());
        guardedTarget->setPixmap(QPixmap::fromImage(image).scaled(
            guardedTarget->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
        guardedTarget->setProperty("thumbnailLoaded", true);
    });
    watcher->setFuture(QtConcurrent::run([access, mediaId, directory,
                                         requestCancel, downloadCancel] {
        const Reply link = request(access, MAI_CLOUD_DRIVE_THUMBNAIL_LINK,
            {{QStringLiteral("video_id"), mediaId}}, requestCancel.get());
        if (!link.ok()) return QImage();
        const QString targetPath = QDir(directory->path()).filePath(
            QUuid::createUuid().toString(QUuid::WithoutBraces) + QStringLiteral(".jpg"));
        const MaiDownloadResult downloaded = downloadCloudFile(access,
            link.body.value(QStringLiteral("thumbnail_url")).toString(), targetPath,
            kMaxThumbnailBytes, 30'000, downloadCancel.get());
        QImage image;
        if (downloaded.status == MAI_DOWNLOAD_OK)
            image = decodeCloudImage(targetPath, 128, 128);
        QFile::remove(targetPath);
        return image;
    }));
}

void CloudDriveDialog::renderVideos(const QJsonArray& videos, bool append) {
    if (!append) {
        clearRows(videoRows_);
        renderFolders();
    }
    if (videos.isEmpty() && !append && videoRows_->count() == 0) {
        auto* empty = new QFrame(this);
        empty->setObjectName(QStringLiteral("cloudDriveEmptyState"));
        empty->setMinimumHeight(220);
        empty->setMaximumHeight(270);
        empty->setStyleSheet(QStringLiteral(
            "QFrame#cloudDriveEmptyState{background:#f9fbfd;border:1px solid #e8eef4;"
            "border-radius:12px;}"));
        auto* layout = new QVBoxLayout(empty);
        layout->setContentsMargins(20, 22, 20, 22);
        layout->setSpacing(8);
        layout->addStretch();
        auto* icon = new QLabel(QStringLiteral("▣"), empty);
        icon->setAlignment(Qt::AlignCenter);
        icon->setFixedSize(56, 56);
        icon->setStyleSheet(QStringLiteral(
            "background:#e6f4fc;color:#1686c8;border-radius:18px;font-size:24px;"));
        layout->addWidget(icon, 0, Qt::AlignHCenter);
        auto* title = new QLabel(currentFolderId_.isEmpty() ?
            QStringLiteral("云盘里还没有图片或视频") : QStringLiteral("此文件夹为空"), empty);
        title->setAlignment(Qt::AlignCenter);
        title->setStyleSheet(QStringLiteral("color:#172033;font-size:17px;font-weight:650;"));
        layout->addWidget(title);
        auto* detail = new QLabel(QStringLiteral("图片最多 20 MB，视频最多 2 GB"), empty);
        detail->setAlignment(Qt::AlignCenter);
        detail->setStyleSheet(QStringLiteral("color:#667085;font-size:13px;"));
        layout->addWidget(detail);
        layout->addStretch();
        videoRows_->addWidget(empty);
    }
    for (const QJsonValue& value : videos) {
        const QJsonObject video = value.toObject();
        const QString id = video.value(QStringLiteral("id")).toString();
        const QString name = video.value(QStringLiteral("name")).toString();
        const bool image = video.value(QStringLiteral("media_type")).toString() ==
            QStringLiteral("image") ||
            video.value(QStringLiteral("content_type")).toString().startsWith(
                QStringLiteral("image/"));
        auto* row = new QFrame(this);
        row->setMinimumHeight(82);
        row->setMaximumHeight(88);
        row->setStyleSheet(QStringLiteral("QFrame{background:#fff;border:1px solid #e6ebf1;border-radius:9px;}"
                                          "QLabel{border:0;}QPushButton{border:0;}"));
        auto* layout = new QHBoxLayout(row);
        auto* thumbnail = new QLabel(image ? QStringLiteral("▣") : QStringLiteral("▶"), row);
        thumbnail->setObjectName(QStringLiteral("cloudDriveThumbnail"));
        thumbnail->setAlignment(Qt::AlignCenter);
        thumbnail->setFixedSize(64, 64);
        thumbnail->setStyleSheet(QStringLiteral(
            "background:#eaf4fc;color:#1686c8;border-radius:9px;font-size:23px;"));
        layout->addWidget(thumbnail);
        auto* labels = new QVBoxLayout;
        auto* title = new QLabel(name, row);
        title->setToolTip(name);
        title->setMinimumWidth(0);
        title->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        title->setStyleSheet(QStringLiteral("font-weight:600;"));
        labels->addWidget(title);
        auto* size = new QLabel((image ? QStringLiteral("图片 · ") :
            QStringLiteral("视频 · ")) +
            humanSize(quint64(video.value(QStringLiteral("size_bytes")).toDouble())), row);
        size->setStyleSheet(QStringLiteral("color:#667085;"));
        labels->addWidget(size);
        layout->addLayout(labels, 1);
        auto* open = new QPushButton(image ? QStringLiteral("查看") : QStringLiteral("播放"), row);
        open->setObjectName(image ? QStringLiteral("cloudDriveViewImage") :
                                    QStringLiteral("cloudDrivePlay"));
        connect(open, &QPushButton::clicked, this, [this, id, name, image] {
            if (image) startImagePreview(id);
            else startPlayback(id, name);
        });
        layout->addWidget(open);
        if (!image) {
            auto* save = new QPushButton(QStringLiteral("保存"), row);
            save->setObjectName(QStringLiteral("cloudDriveSaveVideo"));
            connect(save, &QPushButton::clicked, this,
                    [this, id, name] { saveVideo(id, name); });
            layout->addWidget(save);
        }
        auto* rename = new QPushButton(QStringLiteral("改名"), row);
        rename->setObjectName(QStringLiteral("cloudDriveRenameMedia"));
        connect(rename, &QPushButton::clicked, this,
                [this, id, name] { renameMedia(id, name); });
        layout->addWidget(rename);
        auto* move = new QPushButton(QStringLiteral("移动"), row);
        move->setObjectName(QStringLiteral("cloudDriveMoveMedia"));
        connect(move, &QPushButton::clicked, this,
                [this, id, name] { moveItem(id, name, false); });
        layout->addWidget(move);
        auto* remove = new QPushButton(QStringLiteral("删除"), row);
        remove->setObjectName(QStringLiteral("cloudDriveDelete"));
        connect(remove, &QPushButton::clicked, this,
                [this, id, name] { deleteVideo(id, name); });
        layout->addWidget(remove);
        videoRows_->addWidget(row);
        loadThumbnail(id, thumbnail);
    }
}

void CloudDriveDialog::startUpload(const QString& sourcePath,
                                    const QString& checkpointPath, const QString& originalName) {
    if (busy_ || !sameAccount()) return;
    const QFileInfo file(sourcePath);
    const QString name = originalName.isEmpty() ? file.fileName() : originalName;
    const QByteArray displayName = name.toUtf8();
    MaiCloudDriveMediaInfo media{};
    const bool supported = mai_cloud_drive_media_info(displayName.constData(), &media) == 1;
    const bool image = supported && QByteArray(media.media_type_utf8) == QByteArray("image");
    if (!file.isFile() || !file.isReadable() || file.size() < 1 ||
        !supported || quint64(file.size()) > media.max_size_bytes) {
        setStatus(QStringLiteral("请选择图片（JPEG、PNG、WebP、HEIC，最多 20 MB）"
                                 "或视频（MP4、MOV，最多 2 GB）。"), true);
        return;
    }
    if (checkpointPath.isEmpty() && !AppMessageDialog::confirm(this,
        QStringLiteral("保存到我的云盘"),
        QStringLiteral("将“%1”（%2）保存到当前账号的私人云盘？")
            .arg(name, humanSize(quint64(file.size()))),
        QStringLiteral("开始上传"), false)) return;
    const QString dir = accountDirectory(openedAccountId_);
    if (dir.isEmpty()) { setStatus(QStringLiteral("无法建立云盘任务目录。"), true); return; }
    const QString checkpoint = checkpointPath.isEmpty() ?
        QDir(dir).filePath(QUuid::createUuid().toString(QUuid::WithoutBraces) +
                           QStringLiteral(".maicloud.json")) : checkpointPath;
    // Never accept a checkpoint from another account or an untrusted path.
    if (QFileInfo(checkpoint).absolutePath() != dir) {
        setStatus(QStringLiteral("上传任务不属于当前账号。"), true);
        return;
    }
    const DesktopVideoCircleAccess access = accessProvider_();
    const QString uploadFolderId = currentFolderId_;
    const auto cancel = std::shared_ptr<MaiCloudDriveCancel>(
        mai_cloud_drive_cancel_create(), mai_cloud_drive_cancel_destroy);
    const auto progressState = std::make_shared<ProgressState>();
    requestCancel_ = cancel;
    const unsigned generation = ++generation_;
    setBusy(true, image ? QStringLiteral("正在校验图片并上传…") :
                          QStringLiteral("正在校验视频并上传…"));
    progress_->setValue(0);
    auto* timer = new QTimer(this);
    timer->setInterval(150);
    connect(timer, &QTimer::timeout, this, [this, progressState, generation] {
        if (generation != generation_) return;
        const quint64 total = progressState->total.load(std::memory_order_relaxed);
        const quint64 done = progressState->uploaded.load(std::memory_order_relaxed);
        if (total) {
            progress_->setRange(0, 100);
            progress_->setValue(int((done * 100) / total));
            setStatus(QStringLiteral("已上传 %1 / %2")
                .arg(humanSize(done), humanSize(total)));
        }
    });
    timer->start();
    auto* watcher = new QFutureWatcher<Reply>(this);
    connect(watcher, &QFutureWatcher<Reply>::finished, this,
            [this, watcher, timer, generation] {
        const Reply reply = watcher->result();
        watcher->deleteLater();
        timer->stop();
        timer->deleteLater();
        if (generation != generation_) return;
        requestCancel_.reset();
        setBusy(false);
        if (!sameAccount()) return;
        refreshCheckpoints();
        setStatus(reply.ok() ? QStringLiteral("文件已保存到云盘。") : reply.error, !reply.ok());
        if (reply.ok()) {
            if (reply.uploadedInRoot) {
                currentFolderId_.clear();
                refreshNotice_ = reply.notice;
            }
            refresh();
        }
    });
    const QByteArray contentType(media.content_type_utf8);
    watcher->setFuture(QtConcurrent::run([access, sourcePath, checkpoint, name, contentType,
                                         uploadFolderId,
                                         cancel, progressState] {
        Reply reply;
        const QByteArray base = access.authBaseUrl.toUtf8();
        const QByteArray token = access.accountToken.toUtf8();
        const QByteArray proxy = access.proxyUrl.toUtf8();
        const QByteArray host = access.signedUploadHostSuffix.toUtf8();
        const QByteArray roots = cloudDriveTrustedRoots(access);
        const QByteArray source = sourcePath.toUtf8();
        const QByteArray task = checkpoint.toUtf8();
        const QByteArray display = name.toUtf8();
        MaiCloudDriveOptions options{};
        options.auth_base_url_utf8 = base.constData();
        options.account_token_utf8 = token.constData();
        options.ca_pem = roots.isEmpty() ? nullptr : roots.constData();
        options.ca_pem_length = size_t(roots.size());
        options.proxy_url_utf8 = access.proxyUrl.isNull() ? nullptr : proxy.constData();
        options.signed_upload_host_suffix_utf8 = host.isEmpty() ? nullptr : host.constData();
        MaiCloudDriveResult result{};
        mai_cloud_drive_upload_resumable(&options, source.constData(), task.constData(),
            display.constData(), contentType.constData(), cancel.get(), uploadProgress,
            progressState.get(), &result);
        reply.status = result.status;
        reply.http = result.http_status;
        if (result.response_json_utf8)
            reply.body = QJsonDocument::fromJson(QByteArray(result.response_json_utf8,
                int(result.response_bytes))).object();
        if (!reply.ok()) reply.error = errorText(reply.status, reply.http);
        mai_cloud_drive_result_free(&result);
        if (reply.ok() && !uploadFolderId.isEmpty()) {
            const QString mediaId = reply.body.value(QStringLiteral("video_id")).toString();
            const Reply moved = request(access, MAI_CLOUD_DRIVE_MOVE_MEDIA,
                {{QStringLiteral("media_id"), mediaId},
                 {QStringLiteral("folder_id"), uploadFolderId}}, cancel.get());
            if (!moved.ok()) {
                reply.uploadedInRoot = true;
                reply.notice = QStringLiteral("文件已上传到根目录，但未能移入原文件夹。%1")
                    .arg(moved.error);
            }
        }
        return reply;
    }));
}

void CloudDriveDialog::startPlayback(const QString& videoId, const QString& name) {
    if (busy_ || !sameAccount()) return;
    const DesktopVideoCircleAccess access = accessProvider_();
    const auto cancel = std::shared_ptr<MaiCloudDriveCancel>(
        mai_cloud_drive_cancel_create(), mai_cloud_drive_cancel_destroy);
    const auto vodCancel = std::shared_ptr<MaiCloudVodCancel>(
        mai_cloud_vod_cancel_create(), mai_cloud_vod_cancel_destroy);
    if (!cancel || !vodCancel) {
        setStatus(QStringLiteral("无法准备视频点播，请重试。"), true);
        return;
    }
    requestCancel_ = cancel;
    vodCancel_ = vodCancel;
    const unsigned generation = ++generation_;
    setBusy(true, QStringLiteral("正在连接视频，获取开头片段…"));
    progress_->setRange(0, 0);
    auto* watcher = new QFutureWatcher<Reply>(this);
    connect(watcher, &QFutureWatcher<Reply>::finished, this,
            [this, watcher, generation, name] {
        const Reply reply = watcher->result();
        watcher->deleteLater();
        if (generation != generation_) return;
        requestCancel_.reset();
        vodCancel_.reset();
        setBusy(false);
        if (!sameAccount() || !isVisible()) return;
        if (!reply.ok() || !reply.vod) {
            setStatus(reply.error.isEmpty() ? QStringLiteral("无法打开云盘视频，请重试。") :
                reply.error, true);
            return;
        }
        setStatus(QString());
        if (play_) play_(reply.vod, name);
    });
    watcher->setFuture(QtConcurrent::run([access, videoId, cancel, vodCancel] {
        Reply reply;
        MaiCloudVodResult result{};
        reply.vod = desktopOpenCloudVod(access, videoId, cancel, vodCancel, &result);
        reply.status = reply.vod ? MAI_CLOUD_DRIVE_OK :
            (result.status == MAI_CLOUD_VOD_CANCELED ? MAI_CLOUD_DRIVE_CANCELED :
             MAI_CLOUD_DRIVE_NETWORK_ERROR);
        reply.http = reply.vod ? 206 : result.http_status;
        if (!reply.vod)
            reply.error = result.status == MAI_CLOUD_VOD_CANCELED ?
                QStringLiteral("已取消播放。") : QStringLiteral("视频点播失败，请重试。");
        return reply;
    }));
}

void CloudDriveDialog::saveVideo(const QString& videoId, const QString& name) {
    if (busy_ || !sameAccount()) return;
    const QString fileName = QFileInfo(name).fileName();
    const QString target = QFileDialog::getSaveFileName(
        this, QStringLiteral("保存云盘视频"),
        fileName.isEmpty() ? QStringLiteral("云盘视频.mp4") : fileName,
        QStringLiteral("视频文件 (*.mp4 *.mov *.m4v);;所有文件 (*)"));
    if (target.isEmpty()) return;
    const DesktopVideoCircleAccess access = accessProvider_();
    const auto cancel = std::shared_ptr<MaiCloudDriveCancel>(
        mai_cloud_drive_cancel_create(), mai_cloud_drive_cancel_destroy);
    const auto downloadCancel = std::shared_ptr<MaiDownloadCancel>(
        mai_download_cancel_create(), mai_download_cancel_destroy);
    if (!cancel || !downloadCancel) {
        setStatus(QStringLiteral("无法准备保存，请重试。"), true);
        return;
    }
    requestCancel_ = cancel;
    downloadCancel_ = downloadCancel;
    const unsigned generation = ++generation_;
    setBusy(true, QStringLiteral("正在保存云盘视频…"));
    progress_->setRange(0, 0);
    auto* watcher = new QFutureWatcher<Reply>(this);
    connect(watcher, &QFutureWatcher<Reply>::finished, this,
            [this, watcher, generation, target] {
        const Reply reply = watcher->result();
        watcher->deleteLater();
        if (generation != generation_) return;
        requestCancel_.reset();
        downloadCancel_.reset();
        setBusy(false);
        if (!sameAccount() || !isVisible()) return;
        setStatus(reply.ok() ? QStringLiteral("已保存到 %1").arg(target) :
            (reply.error.isEmpty() ? QStringLiteral("保存失败，请重试。") : reply.error),
            !reply.ok());
    });
    watcher->setFuture(QtConcurrent::run([access, videoId, target, cancel, downloadCancel] {
        Reply reply = request(access, MAI_CLOUD_DRIVE_PLAY_LINK,
            {{QStringLiteral("video_id"), videoId}}, cancel.get());
        if (!reply.ok()) return reply;
        const QString signedUrl = reply.body.value(QStringLiteral("read_url")).toString();
        const qint64 size = reply.body.value(QStringLiteral("size_bytes")).toVariant().toLongLong();
        const QString type = reply.body.value(QStringLiteral("media_type")).toString();
        if (!signedUrl.startsWith(QStringLiteral("https://")) ||
            size <= 0 || quint64(size) > kMaxVideoBytes || type != QStringLiteral("video")) {
            reply.status = MAI_CLOUD_DRIVE_INVALID_ARGUMENT;
            reply.error = QStringLiteral("云盘文件信息无效，无法保存。");
            return reply;
        }
        const MaiDownloadResult downloaded = downloadCloudFile(
            access, signedUrl, target, kMaxVideoBytes, 2 * 60 * 60 * 1000,
            downloadCancel.get());
        if (downloaded.status != MAI_DOWNLOAD_OK) {
            reply.status = downloaded.status == MAI_DOWNLOAD_CANCELED ?
                MAI_CLOUD_DRIVE_CANCELED : MAI_CLOUD_DRIVE_NETWORK_ERROR;
            reply.http = downloaded.http_status;
            reply.error = downloaded.status == MAI_DOWNLOAD_CANCELED ?
                QStringLiteral("已取消保存。") : QStringLiteral("保存失败，请重试。");
        }
        return reply;
    }));
}

void CloudDriveDialog::startImagePreview(const QString& mediaId) {
    if (busy_ || !sameAccount()) return;
    const QString cache = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (cache.isEmpty() || !QDir().mkpath(cache)) {
        setStatus(QStringLiteral("无法建立图片预览缓存目录。"), true);
        return;
    }
    auto temporary = std::make_shared<QTemporaryDir>(
        QDir(cache).filePath(QStringLiteral("CloudDrivePreview-XXXXXX")));
    if (!temporary->isValid()) {
        setStatus(QStringLiteral("无法建立图片预览缓存目录。"), true);
        return;
    }
    const QString target = QDir(temporary->path()).filePath(QStringLiteral("image"));
    const DesktopVideoCircleAccess access = accessProvider_();
    const auto cancel = std::shared_ptr<MaiCloudDriveCancel>(
        mai_cloud_drive_cancel_create(), mai_cloud_drive_cancel_destroy);
    const auto downloadCancel = std::shared_ptr<MaiDownloadCancel>(
        mai_download_cancel_create(), mai_download_cancel_destroy);
    requestCancel_ = cancel;
    downloadCancel_ = downloadCancel;
    const unsigned generation = ++generation_;
    setBusy(true, QStringLiteral("正在打开图片…"));
    progress_->setRange(0, 0);
    auto* watcher = new QFutureWatcher<ImageReply>(this);
    connect(watcher, &QFutureWatcher<ImageReply>::finished, this,
            [this, watcher, generation] {
        const ImageReply image = watcher->result();
        watcher->deleteLater();
        if (generation != generation_) return;
        requestCancel_.reset();
        downloadCancel_.reset();
        setBusy(false);
        if (!sameAccount() || !isVisible()) return;
        if (!image.reply.ok()) { setStatus(image.reply.error, true); return; }
        if (image.image.isNull()) {
            setStatus(QStringLiteral("图片无法解码，请检查文件格式。"), true);
            return;
        }
        setStatus(QString());
        auto* preview = new ImagePreviewDialog(QPixmap::fromImage(image.image), this);
        preview->setObjectName(QStringLiteral("cloudDriveImagePreview"));
        preview->setAttribute(Qt::WA_DeleteOnClose);
        preview->show();
    });
    watcher->setFuture(QtConcurrent::run([access, mediaId, target, temporary,
                                         cancel, downloadCancel] {
        ImageReply image;
        image.reply = request(access, MAI_CLOUD_DRIVE_READ_LINK,
            {{QStringLiteral("video_id"), mediaId}}, cancel.get());
        if (!image.reply.ok()) return image;
        const MaiDownloadResult downloaded = downloadCloudFile(access,
            image.reply.body.value(QStringLiteral("read_url")).toString(), target,
            kMaxImageBytes, 2 * 60 * 1000, downloadCancel.get());
        if (downloaded.status != MAI_DOWNLOAD_OK) {
            image.reply.status = downloaded.status == MAI_DOWNLOAD_CANCELED ?
                MAI_CLOUD_DRIVE_CANCELED : MAI_CLOUD_DRIVE_NETWORK_ERROR;
            image.reply.http = downloaded.http_status;
            image.reply.error = downloaded.status == MAI_DOWNLOAD_CANCELED ?
                QStringLiteral("已取消图片下载。") : QStringLiteral("图片下载失败，请重试。");
        } else {
            image.image = decodeCloudImage(target, 4096, 4096);
        }
        QFile::remove(target);
        return image;
    }));
}

void CloudDriveDialog::renameMedia(const QString& mediaId, const QString& name) {
    if (busy_ || !sameAccount()) return;
    AppTextInputDialog::Options options;
    options.title = QStringLiteral("重命名文件");
    options.initialText = name;
    options.placeholder = QStringLiteral("完整文件名");
    const QString extension = QFileInfo(name).suffix();
    options.description = QStringLiteral("请保留文件扩展名 .%1").arg(extension);
    bool accepted = false;
    const QString renamed = AppTextInputDialog::getText(this, options, &accepted).trimmed();
    if (!accepted || renamed == name) return;
    const QByteArray encoded = renamed.toUtf8();
    if (!mai_cloud_drive_folder_name_valid(encoded.constData()) ||
        QFileInfo(renamed).suffix().compare(extension, Qt::CaseInsensitive) != 0) {
        setStatus(QStringLiteral("请使用有效的完整文件名，并保留扩展名 .%1。")
            .arg(extension), true);
        return;
    }
    mutateFolder(MAI_CLOUD_DRIVE_RENAME_MEDIA,
        {{QStringLiteral("media_id"), mediaId}, {QStringLiteral("name"), renamed}});
}

void CloudDriveDialog::deleteVideo(const QString& videoId, const QString& name) {
    if (busy_ || !sameAccount()) return;
    if (!AppMessageDialog::confirm(this, QStringLiteral("删除云盘文件"),
        QStringLiteral("确定永久删除“%1”？此操作不可撤销。")
            .arg(name), QStringLiteral("删除"), true)) return;
    const DesktopVideoCircleAccess access = accessProvider_();
    const auto cancel = std::shared_ptr<MaiCloudDriveCancel>(
        mai_cloud_drive_cancel_create(), mai_cloud_drive_cancel_destroy);
    requestCancel_ = cancel;
    const unsigned generation = ++generation_;
    setBusy(true, QStringLiteral("正在删除文件…"));
    auto* watcher = new QFutureWatcher<Reply>(this);
    connect(watcher, &QFutureWatcher<Reply>::finished, this, [this, watcher, generation] {
        const Reply reply = watcher->result();
        watcher->deleteLater();
        if (generation != generation_) return;
        requestCancel_.reset();
        setBusy(false);
        if (!sameAccount()) return;
        if (!reply.ok()) { setStatus(reply.error, true); return; }
        refresh();
    });
    watcher->setFuture(QtConcurrent::run([access, videoId, cancel] {
        return request(access, MAI_CLOUD_DRIVE_DELETE_VIDEO,
            {{QStringLiteral("video_id"), videoId}}, cancel.get());
    }));
}

void CloudDriveDialog::discardUpload(const QString& checkpointPath, const QString& name) {
    if (busy_ || !sameAccount()) return;
    if (!AppMessageDialog::confirm(this, QStringLiteral("放弃上传"),
        QStringLiteral("放弃“%1”的续传任务并释放预留空间？")
            .arg(name), QStringLiteral("放弃上传"), true)) return;
    const QString dir = accountDirectory(openedAccountId_);
    if (QFileInfo(checkpointPath).absolutePath() != dir) return;
    const DesktopVideoCircleAccess access = accessProvider_();
    const auto cancel = std::shared_ptr<MaiCloudDriveCancel>(
        mai_cloud_drive_cancel_create(), mai_cloud_drive_cancel_destroy);
    requestCancel_ = cancel;
    const unsigned generation = ++generation_;
    setBusy(true, QStringLiteral("正在放弃上传…"));
    auto* watcher = new QFutureWatcher<Reply>(this);
    connect(watcher, &QFutureWatcher<Reply>::finished, this, [this, watcher, generation] {
        const Reply reply = watcher->result();
        watcher->deleteLater();
        if (generation != generation_) return;
        requestCancel_.reset();
        setBusy(false);
        if (!sameAccount()) return;
        refreshCheckpoints();
        if (!reply.ok()) { setStatus(reply.error, true); return; }
        refresh();
    });
    watcher->setFuture(QtConcurrent::run([access, checkpointPath, cancel] {
        Reply reply;
        const QByteArray base = access.authBaseUrl.toUtf8();
        const QByteArray token = access.accountToken.toUtf8();
        const QByteArray proxy = access.proxyUrl.toUtf8();
        const QByteArray host = access.signedUploadHostSuffix.toUtf8();
        const QByteArray roots = cloudDriveTrustedRoots(access);
        const QByteArray checkpoint = checkpointPath.toUtf8();
        MaiCloudDriveOptions options{};
        options.auth_base_url_utf8 = base.constData();
        options.account_token_utf8 = token.constData();
        options.ca_pem = roots.isEmpty() ? nullptr : roots.constData();
        options.ca_pem_length = size_t(roots.size());
        options.proxy_url_utf8 = access.proxyUrl.isNull() ? nullptr : proxy.constData();
        options.signed_upload_host_suffix_utf8 = host.isEmpty() ? nullptr : host.constData();
        MaiCloudDriveResult result{};
        mai_cloud_drive_abort_resumable(&options, checkpoint.constData(), cancel.get(), &result);
        reply.status = result.status;
        reply.http = result.http_status;
        if (!reply.ok()) reply.error = errorText(reply.status, reply.http);
        mai_cloud_drive_result_free(&result);
        return reply;
    }));
}
