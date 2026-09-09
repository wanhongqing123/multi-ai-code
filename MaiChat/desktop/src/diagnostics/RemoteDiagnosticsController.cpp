#include "diagnostics/RemoteDiagnosticsController.h"
#include "diagnostics/RemoteDiagnosticsProtocol.h"
#include "app/RemoteIMApplication.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimeZone>

namespace {
QString safeId(const QString& value) {
    static const QRegularExpression pattern(QStringLiteral("^[a-zA-Z0-9_.:@/-]{1,200}$"));
    return pattern.match(value).hasMatch() ? value : QStringLiteral("unavailable");
}
QByteArray json(const QJsonObject& object) { return QJsonDocument(object).toJson(QJsonDocument::Indented); }
}

RemoteDiagnosticsController::RemoteDiagnosticsController(RemoteIMApplication* app, QObject* parent)
    : QObject(parent), app_(app), status_(QStringLiteral("选择接收报告的好友后，确认收集并发送。")) {
    timer_.setInterval(100);
    connect(&timer_, &QTimer::timeout, this, &RemoteDiagnosticsController::poll);
    connect(app, &RemoteIMApplication::stateChanged, this, [this] {
        if (running_ && !contextValid()) finish(QStringLiteral("账号或好友已变更，已取消后续回传。"));
    });
    connect(app, &RemoteIMApplication::connectionChanged, this, [this](bool connected) {
        if (running_ && !connected) finish(QStringLiteral("连接已断开，已取消后续回传；已发出的消息无法撤回。"));
    });
}

void RemoteDiagnosticsController::setTimeouts(int responseMs, int sendMs) {
    if (running_) return;
    responseMs_ = qMax(1, responseMs); sendMs_ = qMax(1, sendMs);
}

bool RemoteDiagnosticsController::contextValid() const {
    if (!app_ || !app_->isConnected() || !account_.isValid() ||
        app_->client().currentAccount() != account_ || app_->chatState().ownerUserId() != account_.ownerUserId) return false;
    bool peer = false, recipient = false;
    for (const auto& contact : app_->chatState().contacts()) {
        peer |= contact.userId == peer_; recipient |= contact.userId == recipient_;
    }
    return peer && recipient && recipient_ != account_.ownerUserId;
}

void RemoteDiagnosticsController::start(const QString& peerId, const QString& recipientId) {
    if (running_ || !app_) return;
    account_ = app_->client().currentAccount(); peer_ = peerId; recipient_ = recipientId;
    if (!contextValid()) { finish(QStringLiteral("当前账号、连接或好友已失效，未发起采集。")); return; }
    requestId_ = RemoteDiagnostics::newRequestId();
    const auto generation = ++generation_;
    inspected_.clear(); missingReason_.clear(); requestConfirmed_ = false;
    beganAt_ = QDateTime::currentMSecsSinceEpoch(); originalLocal_ = localEvidence();
    running_ = true; sending_ = false; elapsed_.start(); timer_.start();
    status_ = QStringLiteral("正在请求远端现场，最长等待 60 秒…"); emit changed();
    // Defer the first side effect so immediate cancellation sends nothing.
    QTimer::singleShot(0, this, [this, generation] {
        if (!running_ || generation != generation_ || !contextValid()) return;
        QPointer<RemoteDiagnosticsController> self(this);
        app_->sendDiagnosticTextTo(peer_, RemoteDiagnostics::formatCommand(requestId_), [self, generation](bool ok) {
            if (!self || !self->running_ || self->sending_ || generation != self->generation_) return;
            if (!self->contextValid()) { self->finish(QStringLiteral("账号或好友已变更，已取消回传。")); return; }
            self->requestConfirmed_ = ok;
            if (!ok) self->sendReport({}, QStringLiteral("采集请求发送失败，未取得远端报告。"));
        });
    });
}

void RemoteDiagnosticsController::cancel() {
    if (running_ && !sending_) finish(QStringLiteral("已取消回传；已经发出的采集请求无法撤回。"));
}

void RemoteDiagnosticsController::finish(const QString& status) {
    ++generation_; timer_.stop(); running_ = false; sending_ = false;
    status_ = status; emit changed();
}

void RemoteDiagnosticsController::poll() {
    if (!running_) return;
    if (!contextValid()) { finish(QStringLiteral("账号、连接或好友已变更，已取消回传。")); return; }
    if (sending_) {
        if (sendingElapsed_.elapsed() >= sendMs_)
            finish(QStringLiteral("发送结果尚未确认，请查看排查好友聊天中的消息状态，避免连续重发。"));
        return;
    }
    for (const auto& message : app_->chatState().messagesWith(peer_)) {
        if (message.direction != RemoteIMMessageDirection::Incoming || message.fromUserId != peer_) continue;
        const auto failure = RemoteDiagnostics::parseFailureReceipt(message.text, requestId_);
        if (failure.recognized) { sendReport({}, RemoteDiagnostics::describeFailureReason(failure.reason)); return; }
        if (!message.hasFile || !RemoteDiagnostics::matchesAttachmentFileName(message.file.fileName, requestId_) ||
            inspected_.contains(message.id)) continue;
        const QFileInfo info(message.file.localPath);
        if (!info.exists()) continue;
        inspected_.insert(message.id);
        QFile file(info.absoluteFilePath());
        if (!info.isFile() || info.size() > RemoteDiagnostics::kMaxReportBytes || !file.open(QIODevice::ReadOnly)) {
            missingReason_ = QStringLiteral("收到的报告附件不可读或超过上限，已拒绝合并。"); continue;
        }
        const auto parsed = RemoteDiagnostics::parseReport(file.read(RemoteDiagnostics::kMaxReportBytes + 1),
            message.file.fileName, requestId_, message.fromUserId, peer_);
        if (parsed.accepted) { sendReport(parsed.report, {}); return; }
        missingReason_ = QStringLiteral("收到的报告附件格式或编号无效，已拒绝合并。");
    }
    for (const auto& event : app_->client().diagnosticsEvidence().exportFor(account_, peer_)) {
        if (event.requestId != requestId_) continue;
        if (event.phase == RemoteDiagnostics::AttachmentPhase::DownloadFailed ||
            event.phase == RemoteDiagnostics::AttachmentPhase::WriteFailed) {
            sendReport({}, RemoteDiagnostics::describeAttachmentPhase(event.phase) +
                QStringLiteral("；已收到本次报告的附件通知，本报告仅含本地现场。"));
            return;
        }
    }
    if (!requestConfirmed_ && elapsed_.elapsed() >= sendMs_ && status_ != QStringLiteral("采集请求发送未确认，仍在等待报告…")) {
        status_ = QStringLiteral("采集请求发送未确认，仍在等待报告…"); emit changed();
    }
    if (elapsed_.elapsed() >= responseMs_) {
        auto reason = missingReason_.isEmpty() ? QStringLiteral("等待结束仍未收到有效报告；可能是对方未响应、版本不支持、采集失败或附件下载失败。") : missingReason_;
        if (!requestConfirmed_) reason += QStringLiteral("采集请求发送结果也尚未确认，不能据此断言未送达。");
        sendReport({}, reason);
    }
}

QJsonObject RemoteDiagnosticsController::localEvidence() const {
    QJsonArray messages;
    int loaded = 0;
    app_->chatState().forEachMessageWith(peer_, [&](const RemoteIMMessage& message) {
        ++loaded;
        // Keep observed local rows despite remote clock skew; report their
        // original timestamps rather than treating them as a causality proof.
        messages.append(QJsonObject{{"id", safeId(message.id)}, {"createdAt", double(message.createdAtMillis)},
            {"direction", message.direction == RemoteIMMessageDirection::Incoming ? "incoming" : "outgoing"},
            {"status", int(message.status)}, {"kind", message.hasFile ? "file" : message.hasImage ? "image" : "message"}});
        if (messages.size() > 1000) messages.removeFirst();
    });
    return {{"platform", "Desktop"}, {"appVersion", QCoreApplication::applicationVersion()},
        {"ownerUserID", safeId(account_.ownerUserId)}, {"peerUserID", safeId(peer_)},
        {"collectedAt", double(beganAt_)}, {"timeZone", QString::fromUtf8(QTimeZone::systemTimeZoneId())},
        {"processId", double(QCoreApplication::applicationPid())}, {"messages", messages},
        {"loadedMessageCount", loaded}, {"excludedMessageCount", loaded - messages.size()},
        {"displayedConversation", app_->chatState().selectedPeerId() == peer_},
        {"logCoverage", QStringLiteral("当前账号已加载的最近最多1000条消息元数据与本进程附件阶段；不含全局日志。时间由各端记录，不能据此强行认定因果顺序；选中会话不证明消息像素已显示。")}};
}

void RemoteDiagnosticsController::sendReport(const QJsonObject& remote, const QString& missingReason) {
    if (!running_ || sending_ || !contextValid()) return;
    sending_ = true; sendingElapsed_.start();
    auto local = originalLocal_;
    QJsonArray events;
    for (const auto& event : app_->client().diagnosticsEvidence().exportFor(account_, peer_)) {
        if (event.atMs < beganAt_ - 30 * 60 * 1000) continue;
        QJsonObject entry{{"messageId", safeId(event.messageId)}, {"at", double(event.atMs)},
            {"stage", RemoteDiagnostics::describeAttachmentPhase(event.phase)}, {"code", event.code}};
        if (RemoteDiagnostics::isValidRequestId(event.requestId)) entry.insert("requestId", event.requestId);
        events.append(entry);
    }
    local.insert("attachmentEvents", events);
    QByteArray report = QStringLiteral("# 远程排障报告\n\n排障编号：%1\n\n仅诊断元数据，不含正文、路径或登录凭据。\n\n").arg(requestId_).toUtf8();
    report += "## A 端本地现场\n\n```json\n" + json(local) + "```\n\n";
    if (remote.isEmpty()) report += "## 远端现场缺失\n\n" + missingReason.toUtf8() + "\n";
    else report += "## B 端远程现场\n\n```json\n" + json(remote) + "```\n";
    report += QStringLiteral("\n磁盘文件哈希只反映采集时磁盘文件，不证明运行中的进程加载的是它。sourceCommit 是启动时的来源记录。missing、unavailable、partial、truncated 或没有事件不证明没有故障。\n").toUtf8();
    if (report.size() > 6 * 1024 * 1024) { finish(QStringLiteral("合并报告超过上限，未发送。")); return; }
    QDir dir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/remote-diagnostics"));
    if (!dir.mkpath(".")) { finish(QStringLiteral("无法保存排障报告，未发送。")); return; }
    QFile::setPermissions(dir.absolutePath(), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    int retained = 0;
    for (const auto& info : dir.entryInfoList({QStringLiteral("maichat-diagnostics-*.md")}, QDir::Files | QDir::NoSymLinks)) {
        if (!RemoteDiagnostics::isValidRequestId(info.completeBaseName().mid(QStringLiteral("maichat-diagnostics-").size()))) continue;
        if (info.lastModified().msecsTo(QDateTime::currentDateTime()) > 86400000) QFile::remove(info.absoluteFilePath());
        else ++retained;
    }
    if (retained >= 64) { finish(QStringLiteral("排障报告缓存已满，未生成新附件。")); return; }
    const QString path = dir.filePath(QStringLiteral("maichat-diagnostics-%1.md").arg(requestId_));
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) { finish(QStringLiteral("无法保存排障报告，未发送。")); return; }
    file.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
    if (file.write(report) != report.size() || !file.commit()) { finish(QStringLiteral("排障报告保存失败，未发送。")); return; }
    if (!contextValid()) { finish(QStringLiteral("账号或好友已变更，报告未发送。")); return; }
    status_ = QStringLiteral("正在将%1报告发送给 %2…").arg(remote.isEmpty() ? QStringLiteral("部分") : QStringLiteral("合并"), recipient_); emit changed();
    if (!running_ || !contextValid()) return;
    const auto generation = generation_;
    QPointer<RemoteDiagnosticsController> self(this);
    app_->sendDiagnosticReportTo(recipient_, path, [self, generation, partial = remote.isEmpty()](bool ok) {
        if (!self || !self->running_ || !self->sending_ || generation != self->generation_) return;
        if (!self->contextValid()) { self->finish(QStringLiteral("账号或好友已变更，发送结果未确认。")); return; }
        self->finish(ok ? (partial ? QStringLiteral("部分报告已发送，远端缺失原因已写入报告。") : QStringLiteral("合并报告已发送，缺失和截断项已标注。")) : QStringLiteral("报告发送失败，未确认送达。"));
    });
}
