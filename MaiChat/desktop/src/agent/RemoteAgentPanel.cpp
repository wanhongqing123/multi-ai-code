// 文件作用：呈现远端 Session 的可见记录；输入和权限决定都发往所属设备。
#include "agent/RemoteAgentPanel.h"

#include "MaiCloudDriveClient.h"

#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QIcon>
#include <QJsonArray>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QMimeData>
#include <QPainter>
#include <QPixmap>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QTimer>
#include <QUuid>
#include <QUrl>
#include <QVBoxLayout>

#include "markdown/MarkdownView.h"
#include "ui/ComposerTextEdit.h"
#include "ui/UiZoom.h"

namespace {

QString sessionKey(const QJsonObject& value) {
    return value.value(QStringLiteral("device_slot")).toString() + QLatin1Char(':') +
           value.value(QStringLiteral("login_session_id")).toString() + QLatin1Char(':') +
           value.value(QStringLiteral("id")).toString();
}

QString attachmentSummary(const QStringList& paths) {
    QStringList names;
    for (const QString& path : paths.mid(0, 3)) names.append(QFileInfo(path).fileName());
    return QStringLiteral("待发送 %1 项：%2%3").arg(paths.size()).arg(
        names.join(QStringLiteral("、")),
        paths.size() > 3 ? QStringLiteral(" 等") : QString());
}

QString messageText(const QJsonObject& message) {
    QStringList text;
    int toolCount = 0;
    QString lastTool;
    QString lastState;
    for (const QJsonValue& value : message.value(QStringLiteral("parts")).toArray()) {
        const QJsonObject part = value.toObject();
        const QString kind = part.value(QStringLiteral("kind")).toString();
        if (kind == QStringLiteral("text")) text.append(part.value(QStringLiteral("text")).toString());
        else if (kind == QStringLiteral("tool")) {
            ++toolCount;
            lastTool = part.value(QStringLiteral("tool")).toString();
            lastState = part.value(QStringLiteral("state")).toString();
        } else if (!part.value(QStringLiteral("attachment_id")).toString().isEmpty()) {
            const QString id = part.value(QStringLiteral("attachment_id")).toString();
            QString name = part.value(QStringLiteral("name")).toString(
                QStringLiteral("附件"));
            name.replace(QLatin1Char('['), QLatin1Char(' '));
            name.replace(QLatin1Char(']'), QLatin1Char(' '));
            name.replace(QLatin1Char('\n'), QLatin1Char(' '));
            text.append(QStringLiteral("[下载%1 · %2](maichat-attachment:%3)")
                .arg(kind == QStringLiteral("image") ? QStringLiteral("图片") :
                     kind == QStringLiteral("video") ? QStringLiteral("视频") :
                     kind == QStringLiteral("audio") ? QStringLiteral("音频") :
                     QStringLiteral("文件"), name, id));
        } else if (kind == QStringLiteral("image") || kind == QStringLiteral("video")) {
            text.append(kind == QStringLiteral("image")
                            ? QStringLiteral("[图片在所属设备，暂不可跨设备预览]")
                            : QStringLiteral("[视频在所属设备，暂不可跨设备预览]"));
        }
    }
    if (toolCount > 0) {
        text.append(QStringLiteral("工具调用 %1 次 · %2 · %3")
                        .arg(toolCount).arg(lastTool, lastState));
    }
    return text.join(QStringLiteral("\n\n"));
}

QIcon actionIcon(bool stop, const QColor& color) {
    QPixmap pixmap(48, 48);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(color, 4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    if (stop) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        painter.drawRoundedRect(QRectF(14, 14, 20, 20), 3, 3);
    } else {
        painter.drawLine(QPointF(24, 38), QPointF(24, 10));
        painter.drawLine(QPointF(13, 21), QPointF(24, 10));
        painter.drawLine(QPointF(35, 21), QPointF(24, 10));
    }
    return QIcon(pixmap);
}

class RemotePromptEdit final : public ComposerTextEdit {
public:
    explicit RemotePromptEdit(QWidget* parent) : ComposerTextEdit(parent) {}
    std::function<void()> submit;

protected:
    void keyPressEvent(QKeyEvent* event) override {
        const bool enter = event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter;
        if (enter && !(event->modifiers() & Qt::ShiftModifier)) {
            if (submit) submit();
            return;
        }
        ComposerTextEdit::keyPressEvent(event);
    }
};

}  // namespace

struct RemoteAgentPanel::Runtime {
    QJsonObject session;
    QLabel* status = nullptr;
    QLabel* thinking = nullptr;
    MarkdownView* view = nullptr;
    RemotePromptEdit* editor = nullptr;
    QPushButton* send = nullptr;
    QPushButton* older = nullptr;
    QLabel* attachmentsLabel = nullptr;
    QPushButton* clearAttachments = nullptr;
    QWidget* requests = nullptr;
    QVBoxLayout* requestsLayout = nullptr;
    QTimer historyTimeout;
    QTimer commandTimeout;
    QMap<QString, QJsonObject> messages;
    QMap<QString, QString> rendered;
    QSet<QString> optimisticMessages;
    QString pendingOptimisticId;
    QHash<QString, QString> drafts;
    QHash<QString, QStringList> attachmentDrafts;
    QStringList attachments;
    QString oldest;
    QString pendingDraft;
    QStringList pendingAttachmentPaths;
    bool pendingCommand = false;
    bool lastCommandRejected = false;
    bool historyInFlight = false;
    quint64 historyGeneration = 0;
    bool awaitingResponse = false;
    bool observedBusy = false;
    qint64 commandSentAt = 0;
    QString pendingAction;
    CommandHandler command;
    HistoryHandler history;
    DownloadHandler download;
};

RemoteAgentPanel::RemoteAgentPanel(QWidget* parent) : QWidget(parent), runtime_(std::make_unique<Runtime>()) {
    setObjectName(QStringLiteral("remoteAgentPanel"));
    setStyleSheet(QStringLiteral("background:#ffffff;"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto* header = new QWidget(this);
    header->setObjectName(QStringLiteral("remoteAgentHeader"));
    header->setFixedHeight(UiZoom::s(53));
    auto* headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(UiZoom::s(16), 0, UiZoom::s(16), 0);
    headerLayout->addSpacing(UiZoom::s(34));
    auto* title = new QLabel(QStringLiteral("创作中心"), header);
    title->setObjectName(QStringLiteral("remoteAgentHeaderTitle"));
    QFont titleFont = title->font();
    titleFont.setPixelSize(UiZoom::s(18));
    titleFont.setBold(true);
    title->setFont(titleFont);
    title->setAlignment(Qt::AlignCenter);
    headerLayout->addWidget(title, 1);
    headerLayout->addSpacing(UiZoom::s(34));
    layout->addWidget(header);
    auto* divider = new QWidget(this);
    divider->setObjectName(QStringLiteral("remoteAgentHeaderDivider"));
    divider->setFixedHeight(qMax(1, UiZoom::s(1)));
    divider->setStyleSheet(QStringLiteral(
        "QWidget#remoteAgentHeaderDivider{background:#e2e8f0;}"));
    layout->addWidget(divider);

    runtime_->older = new QPushButton(QStringLiteral("查看更早消息"), this);
    runtime_->older->setFlat(true);
    runtime_->older->hide();
    layout->addWidget(runtime_->older);
    runtime_->view = new MarkdownView(this);
    MarkdownTheme conversationTheme = MarkdownTheme::standard(UiZoom::factor());
    conversationTheme.viewMargin = UiZoom::s(48);
    runtime_->view->setTheme(conversationTheme);
    layout->addWidget(runtime_->view, 1);
    runtime_->requests = new QWidget(this);
    runtime_->requestsLayout = new QVBoxLayout(runtime_->requests);
    runtime_->requestsLayout->setContentsMargins(UiZoom::s(16), 0, UiZoom::s(16), 0);
    runtime_->requests->hide();
    layout->addWidget(runtime_->requests);
    runtime_->status = new QLabel(this);
    runtime_->status->setObjectName(QStringLiteral("remoteAgentStatus"));
    runtime_->status->setWordWrap(true);
    runtime_->status->setContentsMargins(UiZoom::s(16), UiZoom::s(6), UiZoom::s(16), UiZoom::s(6));
    runtime_->status->hide();
    layout->addWidget(runtime_->status);
    auto* composer = new QWidget(this);
    composer->setObjectName(QStringLiteral("remoteAgentComposerPanel"));
    auto* composerLayout = new QVBoxLayout(composer);
    composerLayout->setContentsMargins(UiZoom::s(12), UiZoom::s(2),
                                       UiZoom::s(12), UiZoom::s(8));
    runtime_->editor = new RemotePromptEdit(composer);
    runtime_->editor->setObjectName(QStringLiteral("remoteAgentComposer"));
    runtime_->editor->setAcceptRichText(false);
    runtime_->editor->setPlaceholderText(QStringLiteral("向所属设备发送消息…"));
    runtime_->editor->setFixedHeight(UiZoom::s(136));
    runtime_->editor->setStyleSheet(UiZoom::scaleQss(QStringLiteral(
        "QTextEdit#remoteAgentComposer{border:1px solid #d1d5db;border-radius:14px;"
        "background:#ffffff;color:#172033;padding:10px 52px 10px 13px;}"
        "QTextEdit#remoteAgentComposer:focus{border-color:#b5bbc4;}")));
    QFont editorFont = runtime_->editor->font();
    editorFont.setPixelSize(UiZoom::s(14));
    runtime_->editor->setFont(editorFont);
    runtime_->editor->setMimeHandler([](const QMimeData* mime) {
        return mime && (mime->hasImage() || mime->hasUrls());
    });
    runtime_->editor->submit = [this] { sendOrStop(); };
    auto* attachment = new QPushButton(QStringLiteral("+"), runtime_->editor);
    attachment->setObjectName(QStringLiteral("remoteAgentAttachButton"));
    attachment->setAccessibleName(QStringLiteral("添加会话附件"));
    attachment->setToolTip(attachment->accessibleName());
    attachment->setFixedSize(UiZoom::s(36), UiZoom::s(36));
    attachment->setStyleSheet(QStringLiteral(
        "QPushButton{border:0;border-radius:18px;background:#f1f5f9;"
        "color:#0b67b7;font-size:24px;}"));
    connect(attachment, &QPushButton::clicked, this, [this] {
        const QStringList picked = QFileDialog::getOpenFileNames(this,
            QStringLiteral("选择会话附件"), QDir::homePath());
        for (const QString& path : picked) {
            const QFileInfo file(path);
            const QByteArray name = file.fileName().toUtf8();
            MaiCloudDriveMediaInfo media{};
            if (!file.isFile() || !mai_session_attachment_media_info(name.constData(), &media) ||
                file.size() < 1 || quint64(file.size()) > media.max_size_bytes) {
                showStatus(QStringLiteral("附件格式或大小无效：%1").arg(file.fileName()), true);
                continue;
            }
            if (runtime_->attachments.size() >= 10) {
                showStatus(QStringLiteral("每条消息最多添加 10 项附件"), true);
                break;
            }
            if (!runtime_->attachments.contains(file.absoluteFilePath()))
                runtime_->attachments.append(file.absoluteFilePath());
        }
        runtime_->attachmentsLabel->setText(attachmentSummary(runtime_->attachments));
        runtime_->attachmentsLabel->setToolTip(runtime_->attachments.join(QLatin1Char('\n')));
        runtime_->attachmentsLabel->setVisible(!runtime_->attachments.isEmpty());
        runtime_->clearAttachments->setVisible(!runtime_->attachments.isEmpty());
        updateControls();
    });
    runtime_->editor->setLeadingAction(attachment);
    composerLayout->addWidget(runtime_->editor, 1);
    auto* attachmentsRow = new QWidget(composer);
    auto* attachmentsLayout = new QHBoxLayout(attachmentsRow);
    attachmentsLayout->setContentsMargins(0, 0, 0, 0);
    runtime_->attachmentsLabel = new QLabel(attachmentsRow);
    runtime_->attachmentsLabel->hide();
    attachmentsLayout->addWidget(runtime_->attachmentsLabel, 1);
    runtime_->clearAttachments = new QPushButton(QStringLiteral("清除附件"), attachmentsRow);
    runtime_->clearAttachments->hide();
    attachmentsLayout->addWidget(runtime_->clearAttachments);
    connect(runtime_->clearAttachments, &QPushButton::clicked, this, [this] {
        runtime_->attachments.clear();
        runtime_->attachmentsLabel->hide();
        runtime_->clearAttachments->hide();
        updateControls();
    });
    composerLayout->addWidget(attachmentsRow);
    runtime_->send = new QPushButton(runtime_->editor);
    runtime_->send->setObjectName(QStringLiteral("remoteAgentSendButton"));
    runtime_->send->setCursor(Qt::PointingHandCursor);
    runtime_->send->setFixedSize(UiZoom::s(36), UiZoom::s(36));
    runtime_->send->setIconSize(QSize(UiZoom::s(18), UiZoom::s(18)));
    runtime_->editor->setCornerAction(runtime_->send);
    layout->addWidget(composer);
    connect(runtime_->send, &QPushButton::clicked, this, [this] { sendOrStop(); });
    connect(runtime_->editor, &QTextEdit::textChanged, this, [this] { updateControls(); });
    connect(runtime_->older, &QPushButton::clicked, this, [this] {
        if (!runtime_->oldest.isEmpty()) requestHistory(runtime_->oldest, 20);
    });
    connect(runtime_->view, &MarkdownView::linkActivated, this, [this](const QString& href) {
        const QString prefix = QStringLiteral("maichat-attachment:");
        if (!href.startsWith(prefix) || !runtime_->download) return;
        const QString id = href.mid(prefix.size());
        static const QRegularExpression valid(
            QStringLiteral("^[0-9a-f]{8}(-[0-9a-f]{4}){3}-[0-9a-f]{12}$"));
        if (!valid.match(id).hasMatch()) return;
        const QJsonObject session = runtime_->session;
        showStatus(QStringLiteral("正在下载会话附件…"));
        QPointer<RemoteAgentPanel> guard(this);
        runtime_->download(session, id, [guard, session](const QString& path,
                                                          const QString& error) {
            if (!guard || sessionKey(session) != sessionKey(guard->runtime_->session)) return;
            if (!error.isEmpty()) { guard->showStatus(error, true); return; }
            if (!QDesktopServices::openUrl(QUrl::fromLocalFile(path)))
                guard->showStatus(QStringLiteral("已下载到 %1，无法自动打开").arg(path));
            else guard->showStatus({});
        });
    });
    runtime_->historyTimeout.setSingleShot(true);
    runtime_->historyTimeout.setInterval(30'000);
    connect(&runtime_->historyTimeout, &QTimer::timeout, this, [this] {
        if (!runtime_->historyInFlight) return;
        runtime_->historyInFlight = false;
        runtime_->older->setEnabled(true);
        if (runtime_->messages.isEmpty()) {
            showStatus(QStringLiteral("会话读取超时，可重新进入会话重试"), true);
        }
    });
    runtime_->commandTimeout.setSingleShot(true);
    runtime_->commandTimeout.setInterval(30'000);
    connect(&runtime_->commandTimeout, &QTimer::timeout, this, [this] {
        if (!runtime_->pendingCommand) return;
        runtime_->pendingCommand = false;
        runtime_->lastCommandRejected = false;
        runtime_->awaitingResponse = false;
        removeOptimisticMessage(runtime_->pendingOptimisticId);
        runtime_->pendingOptimisticId.clear();
        runtime_->pendingAction.clear();
        updateThinking();
        if (runtime_->editor->toPlainText().isEmpty() && !runtime_->pendingDraft.isEmpty())
            runtime_->editor->setPlainText(runtime_->pendingDraft);
        runtime_->pendingDraft.clear();
        if (!runtime_->pendingAttachmentPaths.isEmpty()) {
            runtime_->attachments = runtime_->pendingAttachmentPaths;
            runtime_->attachmentsLabel->setText(attachmentSummary(runtime_->attachments));
            runtime_->attachmentsLabel->setVisible(true);
            runtime_->clearAttachments->setVisible(true);
        }
        runtime_->pendingAttachmentPaths.clear();
        showStatus(QStringLiteral("是否执行成功尚未确定，请先查看所属设备的会话记录，不要直接重复发送"), true);
        updateControls();
    });
    updateControls();
}

RemoteAgentPanel::~RemoteAgentPanel() = default;

void RemoteAgentPanel::setCommandHandler(CommandHandler handler) { runtime_->command = std::move(handler); }
void RemoteAgentPanel::setHistoryHandler(HistoryHandler handler) { runtime_->history = std::move(handler); }
void RemoteAgentPanel::setDownloadHandler(DownloadHandler handler) { runtime_->download = std::move(handler); }

QJsonObject RemoteAgentPanel::session() const { return runtime_->session; }

void RemoteAgentPanel::openSession(const QJsonObject& remote) {
    const QString previous = sessionKey(runtime_->session);
    const QString next = sessionKey(remote);
    if (previous != next) {
        ++runtime_->historyGeneration;
        runtime_->historyInFlight = false;
        runtime_->historyTimeout.stop();
        if (!previous.isEmpty()) {
            runtime_->drafts.insert(previous, runtime_->editor->toPlainText());
            runtime_->attachmentDrafts.insert(previous,
                runtime_->attachments.isEmpty() && !runtime_->pendingAttachmentPaths.isEmpty()
                    ? runtime_->pendingAttachmentPaths : runtime_->attachments);
        }
        runtime_->editor->setPlainText(runtime_->drafts.value(next));
        runtime_->attachments = runtime_->attachmentDrafts.value(next);
        runtime_->attachmentsLabel->setText(attachmentSummary(runtime_->attachments));
        runtime_->attachmentsLabel->setToolTip(runtime_->attachments.join(QLatin1Char('\n')));
        runtime_->attachmentsLabel->setVisible(!runtime_->attachments.isEmpty());
        runtime_->clearAttachments->setVisible(!runtime_->attachments.isEmpty());
        runtime_->messages.clear();
        runtime_->rendered.clear();
        runtime_->optimisticMessages.clear();
        runtime_->pendingOptimisticId.clear();
        runtime_->view->clear();
        runtime_->thinking = nullptr;
        runtime_->oldest.clear();
        runtime_->older->hide();
        runtime_->pendingCommand = false;
        runtime_->lastCommandRejected = false;
        runtime_->pendingDraft.clear();
        runtime_->pendingAttachmentPaths.clear();
        runtime_->awaitingResponse = false;
        runtime_->observedBusy = false;
        runtime_->pendingAction.clear();
        runtime_->commandTimeout.stop();
    }
    runtime_->session = remote;
    updateControls();
    if (previous == next && !runtime_->messages.isEmpty()) return;
    showStatus(remote.value(QStringLiteral("online")).toBool()
                   ? QStringLiteral("正在读取所属设备的会话…")
                   : QStringLiteral("所属设备离线；输入保留为草稿"));
    requestHistory({}, 10);
}

void RemoteAgentPanel::updateSession(const QJsonObject& remote) {
    if (sessionKey(remote) != sessionKey(runtime_->session)) return;
    const bool wasOnline = runtime_->session.value(QStringLiteral("online")).toBool();
    runtime_->session = remote;
    updateControls();
    if (remote.value(QStringLiteral("busy")).toBool() && runtime_->awaitingResponse)
        runtime_->observedBusy = true;
    if (!remote.value(QStringLiteral("busy")).toBool() && runtime_->observedBusy) {
        runtime_->awaitingResponse = false;
        runtime_->observedBusy = false;
    }
    updateThinking();
    if (!remote.value(QStringLiteral("online")).toBool()) {
        runtime_->historyInFlight = false;
        runtime_->historyTimeout.stop();
        runtime_->older->setEnabled(true);
        showStatus(QStringLiteral("所属设备离线；输入保留为草稿"));
    } else if (!wasOnline) {
        // WebSocket 断线期间的消息可能没收到；重新上线后补最近一页。
        refreshHistory();
    }
}

void RemoteAgentPanel::refreshHistory() {
    requestHistory({}, 10);
}

void RemoteAgentPanel::requestHistory(const QString& before, int limit) {
    if (!runtime_->history || runtime_->historyInFlight ||
        !runtime_->session.value(QStringLiteral("online")).toBool()) return;
    runtime_->historyInFlight = true;
    runtime_->older->setEnabled(false);
    runtime_->historyTimeout.start();
    const QJsonObject current = runtime_->session;
    const quint64 generation = ++runtime_->historyGeneration;
    QPointer<RemoteAgentPanel> guard(this);
    runtime_->history(current, before, limit, [guard, current, generation](const QString& error) {
        if (!guard || generation != guard->runtime_->historyGeneration ||
            sessionKey(current) != sessionKey(guard->runtime_->session) ||
            error.isEmpty() || !guard->runtime_->historyInFlight) return;
        guard->runtime_->historyInFlight = false;
        guard->runtime_->historyTimeout.stop();
        guard->runtime_->older->setEnabled(true);
        if (guard->runtime_->messages.isEmpty()) guard->showStatus(error, true);
    });
}

void RemoteAgentPanel::sendAction(const QJsonObject& action, bool clearDraft) {
    if (!runtime_->command || runtime_->pendingCommand) return;
    const QJsonObject current = runtime_->session;
    runtime_->pendingCommand = true;
    runtime_->commandTimeout.setInterval(
        action.value(QStringLiteral("local_files")).toArray().isEmpty() ?
            30'000 : 2 * 60 * 60 * 1000);
    runtime_->lastCommandRejected = false;
    runtime_->pendingAction = action.value(QStringLiteral("action")).toString();
    runtime_->commandSentAt = QDateTime::currentMSecsSinceEpoch();
    if (runtime_->pendingAction != QStringLiteral("prompt")) runtime_->awaitingResponse = false;
    runtime_->pendingDraft = clearDraft ? runtime_->editor->toPlainText() : QString();
    runtime_->pendingAttachmentPaths = clearDraft ? runtime_->attachments : QStringList();
    QString optimisticId;
    if (clearDraft && runtime_->pendingAction == QStringLiteral("prompt")) {
        optimisticId = QStringLiteral("msg_z_pending_%1").arg(
            QUuid::createUuid().toString(QUuid::WithoutBraces));
        QString text = action.value(QStringLiteral("text")).toString();
        if (text.isEmpty()) text = QStringLiteral("已发送%1个附件").arg(runtime_->attachments.size());
        runtime_->optimisticMessages.insert(optimisticId);
        runtime_->pendingOptimisticId = optimisticId;
        runtime_->messages.insert(optimisticId,
            {{QStringLiteral("id"), optimisticId},
             {QStringLiteral("role"), QStringLiteral("user")},
             {QStringLiteral("created"), double(runtime_->commandSentAt)},
             {QStringLiteral("parts"), QJsonArray{QJsonObject{
                 {QStringLiteral("kind"), QStringLiteral("text")},
                 {QStringLiteral("text"), text}}}}});
        runtime_->editor->clear();
        renderMessages();
    }
    updateControls();
    runtime_->command(current, action, [this, current, clearDraft, optimisticId](const QString& error) {
        if (sessionKey(current) != sessionKey(runtime_->session)) return;
        if (!error.isEmpty()) {
            runtime_->pendingCommand = false;
            runtime_->pendingAttachmentPaths.clear();
            removeOptimisticMessage(optimisticId);
            if (clearDraft && runtime_->editor->toPlainText().isEmpty())
                runtime_->editor->setPlainText(runtime_->pendingDraft);
            runtime_->pendingDraft.clear();
            runtime_->pendingOptimisticId.clear();
            runtime_->pendingAction.clear();
            runtime_->awaitingResponse = false;
            updateThinking();
            showStatus(error, true);
            updateControls();
            return;
        }
        if (!runtime_->pendingCommand) return;
        if (clearDraft && !runtime_->lastCommandRejected) {
            runtime_->editor->clear();
            runtime_->attachments.clear();
            runtime_->attachmentsLabel->hide();
            runtime_->clearAttachments->hide();
        }
        showStatus(QStringLiteral("已送达所属设备，等待确认执行"));
        runtime_->commandTimeout.start();
    });
}

void RemoteAgentPanel::removeOptimisticMessage(const QString& id) {
    if (id.isEmpty()) return;
    runtime_->optimisticMessages.remove(id);
    runtime_->messages.remove(id);
    runtime_->rendered.remove(id);
    runtime_->view->removeItem(id);
}

void RemoteAgentPanel::sendOrStop() {
    if (runtime_->session.value(QStringLiteral("busy")).toBool()) {
        sendAction({{QStringLiteral("action"), QStringLiteral("stop")}}, false);
        return;
    }
    const QString text = runtime_->editor->toPlainText().trimmed();
    if ((text.isEmpty() && runtime_->attachments.isEmpty()) || text.toUtf8().size() > 16 * 1024) return;
    QJsonArray localFiles;
    for (const QString& path : runtime_->attachments) localFiles.append(path);
    sendAction({{QStringLiteral("action"), QStringLiteral("prompt")},
                {QStringLiteral("text"), text},
                {QStringLiteral("local_files"), localFiles}}, true);
}

void RemoteAgentPanel::receivePayload(const QJsonObject& remote, const QString& kind, const QJsonObject& payload) {
    if (sessionKey(remote) != sessionKey(runtime_->session)) return;
    if (kind == QStringLiteral("event") &&
        payload.value(QStringLiteral("type")).toString() == QStringLiteral("command_result")) {
        runtime_->pendingCommand = false;
        runtime_->commandTimeout.stop();
        const QJsonObject result = payload.value(QStringLiteral("result")).toObject();
        runtime_->lastCommandRejected = !result.value(QStringLiteral("ok")).toBool();
        runtime_->awaitingResponse = !runtime_->lastCommandRejected &&
            runtime_->pendingAction == QStringLiteral("prompt");
        runtime_->observedBusy = false;
        runtime_->pendingAction.clear();
        if (runtime_->lastCommandRejected) {
            removeOptimisticMessage(runtime_->pendingOptimisticId);
            if (!runtime_->pendingAttachmentPaths.isEmpty()) {
                runtime_->attachments = runtime_->pendingAttachmentPaths;
                runtime_->attachmentsLabel->setText(attachmentSummary(runtime_->attachments));
                runtime_->attachmentsLabel->setVisible(true);
                runtime_->clearAttachments->setVisible(true);
            }
        } else if (runtime_->optimisticMessages.contains(runtime_->pendingOptimisticId)) {
            const QString canonicalId = result.value(QStringLiteral("user_message_id")).toString();
            if (!canonicalId.isEmpty()) {
                QJsonObject message = runtime_->messages.value(runtime_->pendingOptimisticId);
                removeOptimisticMessage(runtime_->pendingOptimisticId);
                if (!runtime_->messages.contains(canonicalId)) {
                    message.insert(QStringLiteral("id"), canonicalId);
                    runtime_->messages.insert(canonicalId, message);
                }
                renderMessages();
            }
        }
        runtime_->pendingOptimisticId.clear();
        if (!runtime_->lastCommandRejected && !runtime_->pendingDraft.isEmpty() &&
            runtime_->editor->toPlainText() == runtime_->pendingDraft)
            runtime_->editor->clear();
        if (runtime_->lastCommandRejected && runtime_->editor->toPlainText().isEmpty() &&
            !runtime_->pendingDraft.isEmpty())
            runtime_->editor->setPlainText(runtime_->pendingDraft);
        runtime_->pendingDraft.clear();
        runtime_->pendingAttachmentPaths.clear();
        showStatus(runtime_->lastCommandRejected ?
                       result.value(QStringLiteral("error")).toString() : QString(),
                   runtime_->lastCommandRejected);
        updateThinking();
        updateControls();
        return;
    }
    if (kind == QStringLiteral("history_page")) {
        runtime_->historyInFlight = false;
        runtime_->historyTimeout.stop();
        runtime_->older->setEnabled(true);
    }
    if (!payload.value(QStringLiteral("ok")).toBool()) {
        if (kind == QStringLiteral("history_page"))
            showStatus(payload.value(QStringLiteral("error")).toString(
                           QStringLiteral("所属设备无法读取这段会话")), true);
        return;
    }
    const QJsonArray received = payload.value(QStringLiteral("messages")).toArray();
    for (const QJsonValue& value : received) {
        const QJsonObject message = value.toObject();
        const QString id = message.value(QStringLiteral("id")).toString();
        if (!id.isEmpty()) runtime_->messages.insert(id, message);
        if (runtime_->awaitingResponse &&
            message.value(QStringLiteral("role")).toString() == QStringLiteral("assistant") &&
            message.value(QStringLiteral("created")).toVariant().toLongLong() >=
                runtime_->commandSentAt - 1000 &&
            (!message.value(QStringLiteral("active")).toBool() || !messageText(message).isEmpty()))
            runtime_->awaitingResponse = false;
    }
    if (!received.isEmpty() && kind == QStringLiteral("history_page")) {
        const QString oldest = received.last().toObject().value(QStringLiteral("id")).toString();
        const bool initial = runtime_->oldest.isEmpty();
        if (initial || oldest < runtime_->oldest) {
            runtime_->oldest = oldest;
            runtime_->older->setVisible(received.size() >= (initial ? 10 : 20));
        }
    }
    renderMessages();
    renderRequests(payload);
    if (!runtime_->pendingCommand) showStatus({});
}

void RemoteAgentPanel::renderMessages() {
    const QStringList ids = runtime_->messages.keys();
    const bool followedLatest = runtime_->view->isAtBottom();
    bool prepend = !runtime_->rendered.isEmpty() && !ids.isEmpty() &&
                   ids.first() < runtime_->rendered.firstKey();
    if (!prepend && !runtime_->rendered.isEmpty()) {
        const QString lastRendered = runtime_->rendered.lastKey();
        for (const QString& id : ids)
            if (id < lastRendered && !runtime_->rendered.contains(id)) {
                prepend = true;
                break;
            }
    }
    bool contentChanged = prepend;
    for (const QString& id : ids) {
        const QString content = messageText(runtime_->messages.value(id));
        if (!content.isEmpty() && runtime_->rendered.value(id) != content) {
            contentChanged = true;
            break;
        }
    }
    if (runtime_->thinking && contentChanged) {
        runtime_->thinking->hide();
        runtime_->view->removeItem(QStringLiteral("remote-thinking"));
        runtime_->thinking = nullptr;
    }
    if (prepend) {
        runtime_->view->clear();
        runtime_->rendered.clear();
    }
    runtime_->view->beginBatchUpdates();
    for (const QString& id : ids) {
        const QJsonObject message = runtime_->messages.value(id);
        const QString content = messageText(message);
        if (content.isEmpty() || runtime_->rendered.value(id) == content) continue;
        const MarkdownView::Style style = message.value(QStringLiteral("role")).toString() == QStringLiteral("user")
                                               ? MarkdownView::Style::Bubble
                                               : MarkdownView::Style::Document;
        runtime_->view->addItem(id, style, content);
        runtime_->rendered.insert(id, content);
    }
    runtime_->view->endBatchUpdates();
    updateThinking();
    if (!prepend && followedLatest) runtime_->view->scrollToBottom();
}

void RemoteAgentPanel::updateThinking() {
    const QJsonObject last = runtime_->messages.isEmpty() ? QJsonObject{} : runtime_->messages.last();
    const bool assistantActive = last.value(QStringLiteral("role")).toString() == QStringLiteral("assistant") &&
        last.value(QStringLiteral("active")).toBool();
    const bool visibleAnswer = assistantActive && !messageText(last).trimmed().isEmpty();
    const bool show = (runtime_->awaitingResponse || runtime_->session.value(QStringLiteral("busy")).toBool() ||
                       assistantActive) && !visibleAnswer;
    if (show && !runtime_->thinking) {
        const bool followedLatest = runtime_->view->isAtBottom();
        runtime_->thinking = new QLabel(QStringLiteral("正在思考…"));
        runtime_->thinking->setObjectName(QStringLiteral("remoteThinkingIndicator"));
        runtime_->thinking->setStyleSheet(QStringLiteral("color:#98a2b3;background:transparent;font-size:12px;"));
        runtime_->thinking->setContentsMargins(0, UiZoom::s(2), 0, UiZoom::s(2));
        runtime_->view->addWidget(QStringLiteral("remote-thinking"), runtime_->thinking);
        if (followedLatest) runtime_->view->scrollToBottom();
    } else if (!show && runtime_->thinking) {
        runtime_->thinking->hide();
        runtime_->view->removeItem(QStringLiteral("remote-thinking"));
        runtime_->thinking = nullptr;
    }
}

void RemoteAgentPanel::renderRequests(const QJsonObject& payload) {
    while (QLayoutItem* item = runtime_->requestsLayout->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    for (const QJsonValue& value : payload.value(QStringLiteral("permissions")).toArray()) {
        const QJsonObject permission = value.toObject();
        const QString id = permission.value(QStringLiteral("id")).toString();
        if (id.isEmpty()) continue;
        auto* row = new QWidget(runtime_->requests);
        auto* line = new QHBoxLayout(row);
        line->addWidget(new QLabel(QStringLiteral("请求批准 · %1")
                                       .arg(permission.value(QStringLiteral("tool")).toString()), row), 1);
        const auto addDecision = [&](const QString& title, const QString& decision) {
            auto* button = new QPushButton(title, row);
            line->addWidget(button);
            connect(button, &QPushButton::clicked, this, [this, id, decision] {
                sendAction({{QStringLiteral("action"), QStringLiteral("permission")},
                            {QStringLiteral("id"), id}, {QStringLiteral("decision"), decision}}, false);
            });
        };
        addDecision(QStringLiteral("允许一次"), QStringLiteral("approved"));
        if (permission.value(QStringLiteral("allowForSession")).toBool())
            addDecision(QStringLiteral("本次会话允许"), QStringLiteral("approved_for_session"));
        addDecision(QStringLiteral("拒绝"), QStringLiteral("denied"));
        runtime_->requestsLayout->addWidget(row);
    }
    for (const QJsonValue& value : payload.value(QStringLiteral("questions")).toArray()) {
        const QJsonObject question = value.toObject();
        const QString id = question.value(QStringLiteral("id")).toString();
        if (id.isEmpty()) continue;
        auto* row = new QWidget(runtime_->requests);
        auto* line = new QHBoxLayout(row);
        line->addWidget(new QLabel(question.value(QStringLiteral("question")).toString(), row), 1);
        auto* answer = new QLineEdit(row);
        answer->setPlaceholderText(QStringLiteral("回答问题"));
        line->addWidget(answer);
        auto* button = new QPushButton(QStringLiteral("回答"), row);
        line->addWidget(button);
        connect(button, &QPushButton::clicked, this, [this, id, answer] {
            if (answer->text().trimmed().isEmpty()) return;
            sendAction({{QStringLiteral("action"), QStringLiteral("answer")},
                        {QStringLiteral("id"), id},
                        {QStringLiteral("text"), answer->text().trimmed()}}, false);
        });
        runtime_->requestsLayout->addWidget(row);
    }
    runtime_->requests->setVisible(runtime_->requestsLayout->count() > 0);
}

void RemoteAgentPanel::updateControls() {
    const bool online = runtime_->session.value(QStringLiteral("online")).toBool();
    const bool busy = runtime_->session.value(QStringLiteral("busy")).toBool();
    runtime_->send->setText({});
    runtime_->send->setIcon(actionIcon(busy, busy ? QColor(QStringLiteral("#c24131")) :
                                                QColor(Qt::white)));
    runtime_->send->setToolTip(busy ? QStringLiteral("停止所属设备任务") :
                                     QStringLiteral("发送到所属设备"));
    runtime_->send->setAccessibleName(runtime_->send->toolTip());
    runtime_->send->setStyleSheet(UiZoom::scaleQss(
        busy ? QStringLiteral("QPushButton#remoteAgentSendButton{background:#fff5f3;"
                              "border:1px solid #f0c3bd;border-radius:18px;padding:0;}"
                              "QPushButton#remoteAgentSendButton:hover{background:#fee4e2;}")
             : QStringLiteral("QPushButton#remoteAgentSendButton{background:#0b67b7;"
                              "border:none;border-radius:18px;padding:0;}"
                              "QPushButton#remoteAgentSendButton:hover{background:#095a9f;}"
                              "QPushButton#remoteAgentSendButton:disabled{background:#b8d3e8;}")));
    runtime_->send->setEnabled(online && !runtime_->pendingCommand &&
                               (busy || !runtime_->editor->toPlainText().trimmed().isEmpty() ||
                                !runtime_->attachments.isEmpty()));
}

void RemoteAgentPanel::showStatus(const QString& message, bool failure) {
    runtime_->status->setText(message);
    runtime_->status->setVisible(!message.isEmpty());
    runtime_->status->setStyleSheet(failure ? QStringLiteral("color:#b42318;background:#fff5f3;")
                                            : QStringLiteral("color:#667085;background:transparent;font-size:12px;"));
}
