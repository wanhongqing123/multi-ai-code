#include "ui/VideoCircleDialog.h"

#include "MaiCurlDownload.h"
#include "MaiVideoCircleClient.h"
#if defined(MAICHAT_VIDEO_CIRCLE_MARK_AVAILABLE)
#include "MaiVideoCircleMark.h"
#endif
#include "ui/AppMessageDialog.h"
#include "ui/MessageImageLoader.h"

#include <QCloseEvent>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontMetrics>
#include <QFrame>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPalette>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTabWidget>
#include <QUuid>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

namespace {

constexpr uint64_t kMaxVideoBytes = 200'000'000;

QString errorText(int status, long httpStatus, const QString& fallback = {}) {
    if (!fallback.isEmpty()) return fallback;
    if (httpStatus == 401) return QStringLiteral("登录已过期，请重新登录后重试。");
    if (httpStatus == 404) return QStringLiteral("视频圈暂未开放，或内容已不可访问。");
    if (httpStatus == 429) return QStringLiteral("提交过于频繁，请稍后重试。");
    if (status == MAI_VIDEO_CIRCLE_CANCELED) return QStringLiteral("操作已取消。");
    if (status == MAI_VIDEO_CIRCLE_NETWORK_ERROR) return QStringLiteral("网络连接失败，请稍后重试。");
    return QStringLiteral("操作失败（%1）。").arg(httpStatus > 0 ? QString::number(httpStatus)
                                                        : QStringLiteral("无响应"));
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

QScrollArea* scrollFor(QVBoxLayout** rows, QWidget* parent) {
    auto* scroll = new QScrollArea(parent);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setStyleSheet(QStringLiteral("QScrollArea{background:#ffffff;border:0;}"));
    auto* host = new QWidget(scroll);
    QPalette surface = host->palette();
    surface.setColor(QPalette::Window, QColor(QStringLiteral("#ffffff")));
    host->setPalette(surface);
    host->setAutoFillBackground(true);
    *rows = new QVBoxLayout(host);
    (*rows)->setSizeConstraint(QLayout::SetMinAndMaxSize);
    (*rows)->setContentsMargins(16, 16, 16, 16);
    (*rows)->setSpacing(10);
    scroll->setWidget(host);
    return scroll;
}

QLabel* textLabel(const QString& text, QWidget* parent, bool muted = false) {
    auto* label = new QLabel(text, parent);
    label->setWordWrap(true);
    label->setStyleSheet(muted ? QStringLiteral("color:#667085;")
                               : QStringLiteral("color:#172033;"));
    return label;
}

QWidget* emptyState(const QString& title, const QString& detail, QWidget* parent) {
    auto* host = new QWidget(parent);
    host->setMinimumHeight(300);
    auto* layout = new QVBoxLayout(host);
    layout->setContentsMargins(20, 24, 20, 24);
    layout->addStretch();
    auto* symbol = textLabel(QStringLiteral("▷"), host);
    symbol->setAlignment(Qt::AlignCenter);
    symbol->setFixedSize(58, 58);
    symbol->setStyleSheet(QStringLiteral("background:#e8f4fc;color:#0b83ca;"
                                         "border-radius:18px;font-size:28px;"));
    layout->addWidget(symbol, 0, Qt::AlignHCenter);
    auto* heading = textLabel(title, host);
    heading->setStyleSheet(QStringLiteral("color:#172033;font-size:17px;font-weight:650;"));
    heading->setAlignment(Qt::AlignCenter);
    layout->addWidget(heading);
    auto* explanation = textLabel(detail, host, true);
    explanation->setAlignment(Qt::AlignCenter);
    layout->addWidget(explanation);
    layout->addStretch();
    return host;
}

QString displayDuration(qint64 ms) {
    const qint64 seconds = qMax<qint64>(0, ms / 1000);
    return QStringLiteral("%1:%2").arg(seconds / 60).arg(seconds % 60, 2, 10, QLatin1Char('0'));
}

QString cacheDirectory() {
    QString base = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (base.isEmpty()) base = QDir::tempPath();
    const QString target = QDir(base).filePath(QStringLiteral("VideoCircle"));
    return QDir().mkpath(target) ? target : QString();
}

} // namespace

struct VideoCircleDialog::Reply {
    int status = MAI_VIDEO_CIRCLE_INVALID_ARGUMENT;
    long httpStatus = 0;
    QJsonObject body;
    QString error;
    bool ok() const { return status == MAI_VIDEO_CIRCLE_OK && httpStatus >= 200 && httpStatus < 300; }
};

struct VideoCircleDialog::PublishReply {
    bool ok = false;
    QString error;
    QString status;
};

VideoCircleDialog::Reply VideoCircleDialog::request(const DesktopVideoCircleAccess& access,
                                                    int action, const QJsonObject& payload,
                                                    const MaiVideoCircleCancel* cancel) {
    Reply reply;
    if (!access.isValid()) {
        reply.error = QStringLiteral("请先登录 MaiChat。");
        return reply;
    }
    const QByteArray authBase = access.authBaseUrl.toUtf8();
    const QByteArray mediaBase = access.mediaBaseUrl.toUtf8();
    const QByteArray token = access.accountToken.toUtf8();
    const QByteArray uploadHost = access.signedUploadHostSuffix.toUtf8();
    const QByteArray proxy = access.proxyUrl.toUtf8();
    const QByteArray json = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    MaiVideoCircleOptions options{};
    options.auth_base_url_utf8 = authBase.constData();
    options.media_base_url_utf8 = mediaBase.constData();
    options.account_token_utf8 = token.constData();
    options.ca_pem = access.caPem.isEmpty() ? nullptr : access.caPem.constData();
    options.ca_pem_length = size_t(access.caPem.size());
    options.signed_upload_host_suffix_utf8 = uploadHost.isEmpty() ? nullptr : uploadHost.constData();
    options.proxy_url_utf8 = access.proxyUrl.isNull() ? nullptr : proxy.constData();
    MaiVideoCircleResult result{};
    mai_video_circle_request(&options, MaiVideoCircleAction(action), json.constData(), cancel, &result);
    reply.status = result.status;
    reply.httpStatus = result.http_status;
    if (result.response_json_utf8) {
        reply.body = QJsonDocument::fromJson(QByteArray(result.response_json_utf8,
            int(result.response_bytes))).object();
    }
    if (!reply.ok()) reply.error = errorText(reply.status, reply.httpStatus);
    mai_video_circle_result_free(&result);
    return reply;
}

bool VideoCircleDialog::isAvailable(const DesktopVideoCircleAccess& access) {
    return request(access, MAI_VIDEO_CIRCLE_FEED, {}, nullptr).ok();
}

VideoCircleDialog::VideoCircleDialog(AccessProvider access, AccountIdProvider accountId,
                                     PlayCallback play, QWidget* parent)
    : QDialog(parent), accessProvider_(std::move(access)),
      accountIdProvider_(std::move(accountId)), play_(std::move(play)) {
    setObjectName(QStringLiteral("videoCircleDialog"));
    setWindowTitle(QStringLiteral("AI 视频圈"));
    setMinimumSize(620, 570);
    resize(760, 690);
    setModal(false);
    setStyleSheet(QStringLiteral(
        "QDialog#videoCircleDialog{background:#fff;color:#172033;}"
        "QPushButton{padding:7px 12px;border-radius:8px;background:#eef6fd;color:#0b67b7;border:0;}"
        "QPushButton:hover{background:#dceefb;}"
        "QPushButton:disabled{color:#98a2b3;background:#f4f5f7;}"
        "QLineEdit{padding:8px;border:1px solid #d5dee8;border-radius:8px;}"
        "QTabWidget::pane{border:0;}"
        "QTabBar::tab{background:#f5f8fb;border:0;border-radius:8px;padding:8px 18px;"
        "margin-right:5px;color:#667085;}"
        "QTabBar::tab:selected{background:#e5f3fc;color:#0b67b7;font-weight:600;}"));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(20, 18, 20, 16);
    root->setSpacing(10);
    auto* header = new QHBoxLayout;
    auto* heading = textLabel(QStringLiteral("AI 视频圈"), this);
    heading->setStyleSheet(QStringLiteral("font-size:21px;font-weight:700;color:#172033;"));
    header->addWidget(heading);
    header->addStretch();
    auto* close = new QPushButton(QStringLiteral("关闭"), this);
    close->setObjectName(QStringLiteral("videoCircleClose"));
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    header->addWidget(close);
    root->addLayout(header);
    root->addWidget(textLabel(QStringLiteral("审核通过的视频对其他 MaiChat 用户可见。"), this, true));

    pages_ = new QStackedWidget(this);
    root->addWidget(pages_, 1);
    tabs_ = new QTabWidget(pages_);
    tabs_->setObjectName(QStringLiteral("videoCircleTabs"));
    tabs_->addTab(scrollFor(&feedRows_, tabs_), QStringLiteral("发现"));
    tabs_->addTab(scrollFor(&mineRows_, tabs_), QStringLiteral("我的"));
    tabs_->addTab(scrollFor(&blockedRows_, tabs_), QStringLiteral("已屏蔽"));
    connect(tabs_, &QTabWidget::currentChanged, this, [this](int index) { loadTab(index); });
    pages_->addWidget(tabs_);

    auto* publishPage = new QWidget(pages_);
    auto* publishLayout = new QVBoxLayout(publishPage);
    publishLayout->setContentsMargins(12, 8, 12, 8);
    publishLayout->setSpacing(14);
    auto* back = new QPushButton(QStringLiteral("← 返回视频圈"), publishPage);
    back->setObjectName(QStringLiteral("videoCirclePublishBack"));
    connect(back, &QPushButton::clicked, this, [this] { pages_->setCurrentWidget(tabs_); });
    publishLayout->addWidget(back, 0, Qt::AlignLeft);
    sourcePreview_ = new QLabel(publishPage);
    sourcePreview_->setObjectName(QStringLiteral("videoCircleSourcePreview"));
    sourcePreview_->setFixedSize(360, 205);
    sourcePreview_->setAlignment(Qt::AlignCenter);
    sourcePreview_->setText(QStringLiteral("AI 生成视频"));
    sourcePreview_->setStyleSheet(QStringLiteral("background:#111827;color:white;border-radius:12px;"));
    publishLayout->addWidget(sourcePreview_, 0, Qt::AlignHCenter);
    auto* previewButton = new QPushButton(QStringLiteral("播放预览"), publishPage);
    previewButton->setObjectName(QStringLiteral("videoCirclePreview"));
    connect(previewButton, &QPushButton::clicked, this, [this] {
        if (QFileInfo(sourcePath_).isFile() && play_) play_(sourcePath_);
    });
    publishLayout->addWidget(previewButton, 0, Qt::AlignHCenter);
    authorLabel_ = textLabel(QString(), publishPage, true);
    publishLayout->addWidget(authorLabel_);
    publishLayout->addWidget(textLabel(QStringLiteral("标题"), publishPage));
    title_ = new QLineEdit(publishPage);
    title_->setObjectName(QStringLiteral("videoCircleTitle"));
    title_->setMaxLength(80);
    title_->setPlaceholderText(QStringLiteral("给视频起个标题"));
    publishLayout->addWidget(title_);
    publishLayout->addWidget(textLabel(
        QStringLiteral("视频会生成带有显式和文件元数据标识的新副本；原视频不会更改。"
                       "提交后先由人工审核，审核通过才公开。"), publishPage, true));
    publishButton_ = new QPushButton(QStringLiteral("确认提交审核"), publishPage);
    publishButton_->setObjectName(QStringLiteral("videoCirclePublishConfirm"));
    connect(publishButton_, &QPushButton::clicked, this, [this] { submitPublication(); });
    publishLayout->addWidget(publishButton_, 0, Qt::AlignRight);
    publishLayout->addStretch();
    pages_->addWidget(publishPage);

    status_ = textLabel(QString(), this, true);
    status_->setObjectName(QStringLiteral("videoCircleStatus"));
    root->addWidget(status_);
}

VideoCircleDialog::~VideoCircleDialog() { cancelPending(); }

void VideoCircleDialog::cancelPending() {
    ++generation_;
    if (requestCancel_) mai_video_circle_cancel_request(requestCancel_.get());
    if (downloadCancel_) mai_download_cancel_request(downloadCancel_.get());
#if defined(MAICHAT_VIDEO_CIRCLE_MARK_AVAILABLE)
    if (markCancel_) mai_video_circle_mark_cancel_request(markCancel_.get());
#endif
}

void VideoCircleDialog::closeEvent(QCloseEvent* event) {
    cancelPending();
    setBusy(false);
    QDialog::closeEvent(event);
}

bool VideoCircleDialog::sameAccount() const {
    if (!accountIdProvider_ || !accessProvider_ || openedAccountId_.isEmpty() ||
        accountIdProvider_() != openedAccountId_) return false;
    const DesktopVideoCircleAccess access = accessProvider_();
    return access.isValid() && access.accountId == openedAccountId_;
}

void VideoCircleDialog::setBusy(bool busy, const QString& message) {
    busy_ = busy;
    tabs_->setEnabled(!busy);
    publishButton_->setEnabled(!busy);
    if (moreButton_) moreButton_->setEnabled(!busy);
    if (mineMoreButton_) mineMoreButton_->setEnabled(!busy);
    status_->setStyleSheet(QStringLiteral("color:#667085;"));
    status_->setText(message);
}

void VideoCircleDialog::showError(const QString& message) {
    status_->setStyleSheet(QStringLiteral("color:#b42318;"));
    status_->setText(message);
}

void VideoCircleDialog::openBrowse() {
    if (!accessProvider_ || !accountIdProvider_) return;
    openedAccountId_ = accountIdProvider_();
    if (!sameAccount()) return;
    pages_->setCurrentWidget(tabs_);
    show();
    raise();
    activateWindow();
    loadTab(tabs_->currentIndex());
}

void VideoCircleDialog::openPublish(const QString& completedCardJson,
                                    const QString& sourcePath) {
    if (!accessProvider_ || !accountIdProvider_ || !play_ || busy_) return;
    const QByteArray card = completedCardJson.toUtf8();
    MaiVideoCircleResult eligible{};
    mai_video_circle_inspect_card(card.constData(), &eligible);
    const bool allowed = eligible.status == MAI_VIDEO_CIRCLE_OK;
    mai_video_circle_result_free(&eligible);
    if (!allowed || !QFileInfo(sourcePath).isFile() || !QFileInfo(sourcePath).isAbsolute()) {
        showError(QStringLiteral("这段视频没有可验证的已完成生成记录。"));
        show();
        return;
    }
    openedAccountId_ = accountIdProvider_();
    if (!sameAccount()) return;
    completedCardJson_ = completedCardJson;
    sourcePath_ = sourcePath;
    authorLabel_->setText(QStringLiteral("发布者：%1　·　AI 生成").arg(openedAccountId_));
    title_->clear();
    sourcePreview_->setPixmap(QPixmap());
    sourcePreview_->setText(QStringLiteral("AI 生成视频"));
    MessageImageLoader::instance().load(sourcePath, QSize(720, 410), sourcePreview_,
        [preview = QPointer<QLabel>(sourcePreview_)](const QPixmap& pixels) {
            if (preview) preview->setPixmap(pixels.scaled(preview->size(),
                Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation));
        });
    pages_->setCurrentIndex(1);
    status_->clear();
    show();
    raise();
    activateWindow();
}

void VideoCircleDialog::loadTab(int index, bool append) {
    if (busy_ || !sameAccount() || index < 0 || index > 2) return;
    if (index == 2) append = false;
    const DesktopVideoCircleAccess access = accessProvider_();
    const int action = index == 0 ? MAI_VIDEO_CIRCLE_FEED :
                       index == 1 ? MAI_VIDEO_CIRCLE_MINE : MAI_VIDEO_CIRCLE_BLOCKS_LIST;
    const QString cursor = index == 0 ? nextCursor_ : mineCursor_;
    const QJsonObject payload = append && !cursor.isEmpty()
        ? QJsonObject{{QStringLiteral("before"), cursor}} : QJsonObject{};
    if (!append && index == 0) nextCursor_.clear();
    if (!append && index == 1) mineCursor_.clear();
    const auto cancel = std::shared_ptr<MaiVideoCircleCancel>(
        mai_video_circle_cancel_create(), mai_video_circle_cancel_destroy);
    if (!cancel) return;
    requestCancel_ = cancel;
    const unsigned generation = ++generation_;
    setBusy(true, QStringLiteral("正在加载…"));
    auto* watcher = new QFutureWatcher<Reply>(this);
    connect(watcher, &QFutureWatcher<Reply>::finished, this,
            [this, watcher, generation, index, append] {
        const Reply reply = watcher->result();
        watcher->deleteLater();
        if (generation != generation_) return;
        requestCancel_.reset();
        setBusy(false);
        if (!sameAccount()) { close(); return; }
        if (!reply.ok()) { afterLoadNotice_.clear(); showError(reply.error); return; }
        if (index == 0) {
            nextCursor_ = reply.body.value(QStringLiteral("next_cursor")).toString();
            renderFeed(reply.body.value(QStringLiteral("posts")).toArray(), append);
        } else if (index == 1) {
            mineCursor_ = reply.body.value(QStringLiteral("next_cursor")).toString();
            renderMine(reply.body.value(QStringLiteral("posts")).toArray(), append);
        } else {
            renderBlocks(reply.body.value(QStringLiteral("authors")).toArray());
        }
        if (!afterLoadNotice_.isEmpty()) {
            status_->setText(afterLoadNotice_);
            afterLoadNotice_.clear();
        }
    });
    watcher->setFuture(QtConcurrent::run([access, action, payload, cancel] {
        return request(access, action, payload, cancel.get());
    }));
}

void VideoCircleDialog::renderFeed(const QJsonArray& posts, bool append) {
    if (!append) clearRows(feedRows_);
    else {
        if (moreButton_) {
            const int index = feedRows_->indexOf(moreButton_);
            if (index >= 0) delete feedRows_->takeAt(index);
        }
        if (feedRows_->count() > 0 && feedRows_->itemAt(feedRows_->count() - 1)->spacerItem())
            delete feedRows_->takeAt(feedRows_->count() - 1);
    }
    if (moreButton_) { moreButton_->hide(); moreButton_->deleteLater(); moreButton_ = nullptr; }
    if (posts.isEmpty() && !append) feedRows_->addWidget(emptyState(
        QStringLiteral("还没有审核通过的视频"),
        QStringLiteral("新作品审核通过后会出现在这里。"), feedRows_->parentWidget()));
    for (const QJsonValue& value : posts) {
        const QJsonObject post = value.toObject();
        const QString id = post.value(QStringLiteral("id")).toString();
        const QString author = post.value(QStringLiteral("author_id")).toString();
        auto* row = new QFrame(feedRows_->parentWidget());
        row->setObjectName(QStringLiteral("videoCircleFeedRow"));
        row->setFixedHeight(112);
        row->setStyleSheet(QStringLiteral(
            "QFrame#videoCircleFeedRow{background:#f7fafd;"
            "border:1px solid #e2e8f0;border-radius:12px;}"));
        auto* layout = new QVBoxLayout(row);
        layout->setContentsMargins(14, 12, 14, 12);
        layout->setSpacing(5);
        const QString fullTitle = post.value(QStringLiteral("title")).toString();
        auto* title = textLabel(fullTitle, row);
        title->setStyleSheet(QStringLiteral("font-size:16px;font-weight:650;color:#172033;"));
        title->setWordWrap(false);
        title->setFixedHeight(21);
        title->setToolTip(fullTitle);
        title->setText(title->fontMetrics().elidedText(fullTitle, Qt::ElideRight,
            qMin(560, qMax(300, width() - 160))));
        layout->addWidget(title);
        auto* byline = textLabel(QStringLiteral("%1　·　AI 生成　·　%2")
            .arg(post.value(QStringLiteral("author_name")).toString(author),
                 displayDuration(post.value(QStringLiteral("duration_ms")).toVariant().toLongLong())), row, true);
        byline->setWordWrap(false);
        byline->setFixedHeight(19);
        layout->addWidget(byline);
        auto* actions = new QHBoxLayout;
        auto* play = new QPushButton(QStringLiteral("播放"), row);
        connect(play, &QPushButton::clicked, this, [this, id] { playPost(id); });
        actions->addWidget(play);
        if (author != openedAccountId_) {
            auto* report = new QPushButton(QStringLiteral("举报"), row);
            connect(report, &QPushButton::clicked, this, [this, id] {
                bool accepted = false;
                const QStringList labels{QStringLiteral("垃圾内容"), QStringLiteral("骚扰"),
                    QStringLiteral("色情"), QStringLiteral("暴力"), QStringLiteral("版权"),
                    QStringLiteral("其他")};
                const QStringList values{QStringLiteral("spam"), QStringLiteral("harassment"),
                    QStringLiteral("sexual"), QStringLiteral("violence"),
                    QStringLiteral("copyright"), QStringLiteral("other")};
                const QString chosen = QInputDialog::getItem(this, QStringLiteral("举报视频"),
                    QStringLiteral("请选择原因"), labels, 0, false, &accepted);
                if (!accepted) return;
                const int reason = labels.indexOf(chosen);
                if (reason >= 0) runMutation(MAI_VIDEO_CIRCLE_REPORT,
                    {{QStringLiteral("post_id"), id}, {QStringLiteral("reason"), values.at(reason)}},
                    QStringLiteral("举报已提交"), -1);
            });
            actions->addWidget(report);
            auto* block = new QPushButton(QStringLiteral("屏蔽作者"), row);
            connect(block, &QPushButton::clicked, this, [this, author] {
                if (AppMessageDialog::confirm(this, QStringLiteral("屏蔽作者"),
                        QStringLiteral("屏蔽后将不再看到该作者的视频。"), QStringLiteral("屏蔽"),
                        /*destructive=*/false))
                    runMutation(MAI_VIDEO_CIRCLE_BLOCK, {{QStringLiteral("author_id"), author}},
                                QStringLiteral("已屏蔽"), 0);
            });
            actions->addWidget(block);
        }
        actions->addStretch();
        layout->addLayout(actions);
        feedRows_->addWidget(row);
        row->show();
    }
    if (!nextCursor_.isEmpty()) {
        moreButton_ = new QPushButton(QStringLiteral("加载更多"), feedRows_->parentWidget());
        moreButton_->setObjectName(QStringLiteral("videoCircleLoadMore"));
        connect(moreButton_, &QPushButton::clicked, this, [this] { loadTab(0, true); });
        feedRows_->addWidget(moreButton_, 0, Qt::AlignHCenter);
        moreButton_->show();
    }
    feedRows_->addStretch();
}

void VideoCircleDialog::renderMine(const QJsonArray& posts, bool append) {
    if (!append) clearRows(mineRows_);
    else {
        if (mineMoreButton_) {
            const int index = mineRows_->indexOf(mineMoreButton_);
            if (index >= 0) delete mineRows_->takeAt(index);
        }
        if (mineRows_->count() > 0 && mineRows_->itemAt(mineRows_->count() - 1)->spacerItem())
            delete mineRows_->takeAt(mineRows_->count() - 1);
    }
    if (mineMoreButton_) {
        mineMoreButton_->hide();
        mineMoreButton_->deleteLater();
        mineMoreButton_ = nullptr;
    }
    if (posts.isEmpty() && !append) mineRows_->addWidget(emptyState(
        QStringLiteral("还没有提交视频"),
        QStringLiteral("从创作中心已完成的 AI 视频卡片发布。"), mineRows_->parentWidget()));
    for (const QJsonValue& value : posts) {
        const QJsonObject post = value.toObject();
        const QString id = post.value(QStringLiteral("id")).toString();
        const QString state = post.value(QStringLiteral("status")).toString();
        const QString label = state == QStringLiteral("pending") ? QStringLiteral("待审核") :
                              state == QStringLiteral("approved") ? QStringLiteral("已公开") :
                              state == QStringLiteral("rejected") ? QStringLiteral("未通过") :
                              state == QStringLiteral("hidden") ? QStringLiteral("已下架") :
                              state == QStringLiteral("withdrawn") ? QStringLiteral("已撤回") :
                              QStringLiteral("状态待更新");
        auto* row = new QFrame(mineRows_->parentWidget());
        row->setObjectName(QStringLiteral("videoCircleMineRow"));
        row->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);
        row->setMaximumHeight(112);
        row->setStyleSheet(QStringLiteral(
            "QFrame#videoCircleMineRow{background:#f7fafd;"
            "border:1px solid #e2e8f0;border-radius:12px;}"));
        auto* layout = new QHBoxLayout(row);
        layout->setContentsMargins(14, 12, 14, 12);
        auto* details = new QWidget(row);
        auto* detailsLayout = new QVBoxLayout(details);
        detailsLayout->setContentsMargins(0, 0, 0, 0);
        detailsLayout->setSpacing(3);
        auto* name = textLabel(post.value(QStringLiteral("title")).toString() +
                               QStringLiteral("　·　") + label, row);
        detailsLayout->addWidget(name);
        const QJsonValue reasonValue = post.value(QStringLiteral("review_reason"));
        if (reasonValue.isString() && !reasonValue.toString().trimmed().isEmpty()) {
            auto* reason = textLabel(reasonValue.toString(), details);
            reason->setObjectName(QStringLiteral("videoCircleReviewReason"));
            reason->setStyleSheet(QStringLiteral("color:#b42318;"));
            reason->setMaximumHeight(48);
            detailsLayout->addWidget(reason);
        }
        layout->addWidget(details, 1);
        if (state == QStringLiteral("pending") || state == QStringLiteral("approved")) {
            auto* play = new QPushButton(QStringLiteral("播放"), row);
            connect(play, &QPushButton::clicked, this, [this, id] { playPost(id); });
            layout->addWidget(play);
        }
        if (state != QStringLiteral("withdrawn")) {
            auto* withdraw = new QPushButton(QStringLiteral("撤回"), row);
            connect(withdraw, &QPushButton::clicked, this, [this, id] {
                if (AppMessageDialog::confirm(this, QStringLiteral("撤回视频"),
                        QStringLiteral("撤回后其他用户将无法播放，且不能恢复。"),
                        QStringLiteral("撤回"), true))
                    runMutation(MAI_VIDEO_CIRCLE_WITHDRAW,
                                {{QStringLiteral("post_id"), id}}, QStringLiteral("已撤回"), 1);
            });
            layout->addWidget(withdraw);
        }
        mineRows_->addWidget(row);
        row->show();
    }
    if (!mineCursor_.isEmpty()) {
        mineMoreButton_ = new QPushButton(QStringLiteral("加载更多"), mineRows_->parentWidget());
        mineMoreButton_->setObjectName(QStringLiteral("videoCircleMineLoadMore"));
        connect(mineMoreButton_, &QPushButton::clicked, this, [this] { loadTab(1, true); });
        mineRows_->addWidget(mineMoreButton_, 0, Qt::AlignHCenter);
        mineMoreButton_->show();
    }
    mineRows_->addStretch();
}

void VideoCircleDialog::renderBlocks(const QJsonArray& authors) {
    clearRows(blockedRows_);
    if (authors.isEmpty()) blockedRows_->addWidget(emptyState(
        QStringLiteral("没有屏蔽的作者"),
        QStringLiteral("你屏蔽的作者会列在这里。"), blockedRows_->parentWidget()));
    for (const QJsonValue& value : authors) {
        const QJsonObject author = value.toObject();
        const QString id = author.value(QStringLiteral("author_id")).toString();
        auto* row = new QFrame(blockedRows_->parentWidget());
        row->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);
        row->setMaximumHeight(72);
        auto* layout = new QHBoxLayout(row);
        layout->addWidget(textLabel(author.value(QStringLiteral("author_name")).toString(id), row), 1);
        auto* unblock = new QPushButton(QStringLiteral("取消屏蔽"), row);
        connect(unblock, &QPushButton::clicked, this, [this, id] {
            runMutation(MAI_VIDEO_CIRCLE_UNBLOCK, {{QStringLiteral("author_id"), id}},
                        QStringLiteral("已取消屏蔽"), 2);
        });
        layout->addWidget(unblock);
        blockedRows_->addWidget(row);
        row->show();
    }
    blockedRows_->addStretch();
}

void VideoCircleDialog::runMutation(int action, const QJsonObject& payload,
                                    const QString& success, int refreshTab) {
    if (busy_ || !sameAccount()) return;
    const DesktopVideoCircleAccess access = accessProvider_();
    const auto cancel = std::shared_ptr<MaiVideoCircleCancel>(
        mai_video_circle_cancel_create(), mai_video_circle_cancel_destroy);
    if (!cancel) return;
    requestCancel_ = cancel;
    const unsigned generation = ++generation_;
    setBusy(true, QStringLiteral("正在处理…"));
    auto* watcher = new QFutureWatcher<Reply>(this);
    connect(watcher, &QFutureWatcher<Reply>::finished, this,
            [this, watcher, generation, action, success, refreshTab] {
        const Reply reply = watcher->result();
        watcher->deleteLater();
        if (generation != generation_) return;
        requestCancel_.reset();
        setBusy(false);
        if (!sameAccount()) { close(); return; }
        if (!reply.ok()) { showError(reply.error); return; }
        const QString notice = action == MAI_VIDEO_CIRCLE_WITHDRAW &&
                reply.body.value(QStringLiteral("cleanup_pending")).toBool()
            ? QStringLiteral("已从列表移除，文件清理中。") : success;
        if (refreshTab >= 0) {
            afterLoadNotice_ = notice;
            loadTab(refreshTab);
        } else {
            status_->setText(notice);
        }
    });
    watcher->setFuture(QtConcurrent::run([access, action, payload, cancel] {
        return request(access, action, payload, cancel.get());
    }));
}

void VideoCircleDialog::playPost(const QString& postId) {
    if (busy_ || !sameAccount() || !play_) return;
    const DesktopVideoCircleAccess access = accessProvider_();
    const QString directory = cacheDirectory();
    if (directory.isEmpty()) { showError(QStringLiteral("无法建立视频缓存目录。")); return; }
    const QString target = QDir(directory).filePath(QUuid::createUuid().toString(QUuid::WithoutBraces) +
                                                    QStringLiteral(".mp4"));
    const auto cancel = std::shared_ptr<MaiVideoCircleCancel>(
        mai_video_circle_cancel_create(), mai_video_circle_cancel_destroy);
    const auto downloadCancel = std::shared_ptr<MaiDownloadCancel>(
        mai_download_cancel_create(), mai_download_cancel_destroy);
    if (!cancel || !downloadCancel) return;
    requestCancel_ = cancel;
    downloadCancel_ = downloadCancel;
    const unsigned generation = ++generation_;
    setBusy(true, QStringLiteral("正在准备播放…"));
    auto* watcher = new QFutureWatcher<Reply>(this);
    connect(watcher, &QFutureWatcher<Reply>::finished, this,
            [this, watcher, generation, target] {
        const Reply reply = watcher->result();
        watcher->deleteLater();
        if (generation != generation_) { QFile::remove(target); return; }
        requestCancel_.reset();
        downloadCancel_.reset();
        setBusy(false);
        if (!sameAccount()) { QFile::remove(target); close(); return; }
        if (!reply.ok()) { showError(reply.error); return; }
        hide();
        play_(target);
    });
    watcher->setFuture(QtConcurrent::run([access, postId, target, cancel, downloadCancel] {
        Reply result = request(access, MAI_VIDEO_CIRCLE_PLAY_LINK,
                               {{QStringLiteral("post_id"), postId}}, cancel.get());
        if (!result.ok()) return result;
        const QString url = result.body.value(QStringLiteral("read_url")).toString();
        if (!url.startsWith(QStringLiteral("https://"))) {
            result.error = QStringLiteral("播放链接无效。");
            result.status = MAI_VIDEO_CIRCLE_INVALID_RESPONSE;
            return result;
        }
        const QByteArray urlBytes = url.toUtf8();
        const QByteArray targetBytes = target.toUtf8();
        const QByteArray proxy = access.proxyUrl.toUtf8();
        MaiDownloadOptions options{};
        options.url_utf8 = urlBytes.constData();
        options.target_path_utf8 = targetBytes.constData();
        options.max_bytes = kMaxVideoBytes;
        options.ca_pem = access.caPem.isEmpty() ? nullptr : access.caPem.constData();
        options.ca_pem_length = size_t(access.caPem.size());
        options.proxy_url_utf8 = access.proxyUrl.isNull() ? nullptr : proxy.constData();
        MaiDownloadResult downloaded{};
        mai_download_file(&options, downloadCancel.get(), &downloaded);
        if (downloaded.status != MAI_DOWNLOAD_OK) {
            result.status = MAI_VIDEO_CIRCLE_NETWORK_ERROR;
            result.httpStatus = downloaded.http_status;
            result.error = QStringLiteral("视频下载失败，请重试。");
        }
        return result;
    }));
}

void VideoCircleDialog::submitPublication() {
    if (busy_ || !sameAccount()) return;
#if !defined(MAICHAT_VIDEO_CIRCLE_MARK_AVAILABLE)
    showError(QStringLiteral("当前构建没有视频标识能力，无法发布。"));
    return;
#else
    const QString title = title_->text().trimmed();
    if (title.isEmpty()) { showError(QStringLiteral("请填写标题。")); title_->setFocus(); return; }
    if (!QFileInfo(sourcePath_).isFile()) { showError(QStringLiteral("原视频文件已不存在。")); return; }
    if (!AppMessageDialog::confirm(this, QStringLiteral("提交 AI 视频"),
            QStringLiteral("将“%1”作为 %2 发布？审核通过后其他用户可看到。")
                .arg(title, openedAccountId_), QStringLiteral("提交审核"),
                /*destructive=*/false)) return;
    const DesktopVideoCircleAccess access = accessProvider_();
    const QString source = sourcePath_;
    const QString card = completedCardJson_;
    const QString directory = cacheDirectory();
    if (directory.isEmpty()) { showError(QStringLiteral("无法建立临时视频目录。")); return; }
    const QString marked = QDir(directory).filePath(QUuid::createUuid().toString(QUuid::WithoutBraces) +
                                                     QStringLiteral(".mp4"));
    const auto cancel = std::shared_ptr<MaiVideoCircleCancel>(
        mai_video_circle_cancel_create(), mai_video_circle_cancel_destroy);
    const auto markCancel = std::shared_ptr<MaiVideoCircleMarkCancel>(
        mai_video_circle_mark_cancel_create(), mai_video_circle_mark_cancel_destroy);
    if (!cancel || !markCancel) return;
    requestCancel_ = cancel;
    markCancel_ = markCancel;
    const unsigned generation = ++generation_;
    setBusy(true, QStringLiteral("正在生成标识并提交审核…"));
    auto* watcher = new QFutureWatcher<PublishReply>(this);
    connect(watcher, &QFutureWatcher<PublishReply>::finished, this,
            [this, watcher, generation] {
        const PublishReply result = watcher->result();
        watcher->deleteLater();
        if (generation != generation_) return;
        requestCancel_.reset();
        markCancel_.reset();
        setBusy(false);
        if (!sameAccount()) { close(); return; }
        if (!result.ok) { showError(result.error); return; }
        completedCardJson_.clear();
        sourcePath_.clear();
        pages_->setCurrentWidget(tabs_);
        tabs_->setCurrentIndex(1);
        setBusy(false, QStringLiteral("已提交，等待人工审核。"));
        loadTab(1);
    });
    watcher->setFuture(QtConcurrent::run([access, source, card, title, marked, cancel, markCancel] {
        PublishReply published;
        if (!isAvailable(access)) {
            published.error = QStringLiteral("视频圈暂未开放或登录已过期，请稍后重试。");
            return published;
        }
        const QByteArray sourceBytes = source.toUtf8();
        const QByteArray markedBytes = marked.toUtf8();
        MaiVideoCircleMarkResult mark{};
        mai_video_circle_prepare_marked_mp4(sourceBytes.constData(), markedBytes.constData(),
                                            markCancel.get(), &mark);
        if (mark.status != MAI_VIDEO_CIRCLE_MARK_OK || !mark.visible_mark_verified ||
            !mark.metadata_mark_verified) {
            published.error = QStringLiteral("视频标识制作失败（%1）。").arg(QString::fromLatin1(mark.detail_utf8));
            return published;
        }
        const auto cleanup = [&marked] { QFile::remove(marked); };
        const QJsonObject signPayload{{QStringLiteral("filename"), QFileInfo(marked).fileName()},
                                      {QStringLiteral("size_bytes"), qint64(mark.size_bytes)}};
        Reply signedUpload = request(access, MAI_VIDEO_CIRCLE_SIGN_UPLOAD, signPayload, cancel.get());
        if (!signedUpload.ok()) { published.error = signedUpload.error; cleanup(); return published; }
        const QString objectKey = signedUpload.body.value(QStringLiteral("object_key")).toString();
        const QByteArray authBase = access.authBaseUrl.toUtf8();
        const QByteArray mediaBase = access.mediaBaseUrl.toUtf8();
        const QByteArray token = access.accountToken.toUtf8();
        const QByteArray uploadHost = access.signedUploadHostSuffix.toUtf8();
        const QByteArray proxy = access.proxyUrl.toUtf8();
        const QByteArray signedJson = QJsonDocument(signedUpload.body).toJson(QJsonDocument::Compact);
        MaiVideoCircleOptions options{};
        options.auth_base_url_utf8 = authBase.constData();
        options.media_base_url_utf8 = mediaBase.constData();
        options.account_token_utf8 = token.constData();
        options.ca_pem = access.caPem.isEmpty() ? nullptr : access.caPem.constData();
        options.ca_pem_length = size_t(access.caPem.size());
        options.signed_upload_host_suffix_utf8 = uploadHost.isEmpty() ? nullptr : uploadHost.constData();
        options.proxy_url_utf8 = access.proxyUrl.isNull() ? nullptr : proxy.constData();
        MaiVideoCircleResult upload{};
        mai_video_circle_upload_file(&options, signedJson.constData(), markedBytes.constData(),
                                     mark.size_bytes, cancel.get(), &upload);
        const bool uploaded = upload.status == MAI_VIDEO_CIRCLE_OK;
        const QString uploadError = errorText(upload.status, upload.http_status);
        mai_video_circle_result_free(&upload);
        if (!uploaded) { published.error = uploadError; cleanup(); return published; }
        const QJsonObject payload{{QStringLiteral("completed_card"), QJsonDocument::fromJson(card.toUtf8()).object()},
                                  {QStringLiteral("title"), title},
                                  {QStringLiteral("object_key"), objectKey},
                                  {QStringLiteral("size_bytes"), qint64(mark.size_bytes)},
                                  {QStringLiteral("duration_ms"), qint64(mark.duration_ms)}};
        const Reply submitted = request(access, MAI_VIDEO_CIRCLE_PUBLISH_DRAFT, payload, cancel.get());
        cleanup();
        if (!submitted.ok()) { published.error = submitted.error; return published; }
        published.status = submitted.body.value(QStringLiteral("status")).toString();
        published.ok = published.status == QStringLiteral("pending");
        if (!published.ok) published.error = QStringLiteral("服务器没有确认待审核状态。");
        return published;
    }));
#endif
}
