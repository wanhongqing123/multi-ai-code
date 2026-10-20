// 文件作用：呈现远端 Session 的可见记录；输入和权限决定都发往所属设备。
#include "agent/RemoteAgentPanel.h"

#include <QDateTime>
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
#include <QTimer>
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
    QLabel* device = nullptr;
    QLabel* status = nullptr;
    QLabel* thinking = nullptr;
    MarkdownView* view = nullptr;
    RemotePromptEdit* editor = nullptr;
    QPushButton* send = nullptr;
    QPushButton* older = nullptr;
    QWidget* requests = nullptr;
    QVBoxLayout* requestsLayout = nullptr;
    QTimer refresh;
    QTimer historyTimeout;
    QTimer commandTimeout;
    QMap<QString, QJsonObject> messages;
    QMap<QString, QString> rendered;
    QHash<QString, QString> drafts;
    QString oldest;
    QString pendingDraft;
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
};

RemoteAgentPanel::RemoteAgentPanel(QWidget* parent) : QWidget(parent), runtime_(std::make_unique<Runtime>()) {
    setObjectName(QStringLiteral("remoteAgentPanel"));
    setStyleSheet(QStringLiteral("background:#ffffff;"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto* header = new QWidget(this);
    header->setFixedHeight(UiZoom::s(54));
    header->setStyleSheet(QStringLiteral("border-bottom:1px solid #e2e8f0;"));
    auto* headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(UiZoom::s(18), 0, UiZoom::s(18), 0);
    auto* title = new QLabel(QStringLiteral("创作中心"), header);
    QFont titleFont = title->font();
    titleFont.setPixelSize(UiZoom::s(18));
    titleFont.setBold(true);
    title->setFont(titleFont);
    headerLayout->addWidget(title);
    headerLayout->addStretch();
    runtime_->device = new QLabel(header);
    runtime_->device->setStyleSheet(QStringLiteral("color:#667085;"));
    headerLayout->addWidget(runtime_->device);
    layout->addWidget(header);

    runtime_->older = new QPushButton(QStringLiteral("查看更早消息"), this);
    runtime_->older->setFlat(true);
    runtime_->older->hide();
    layout->addWidget(runtime_->older);
    runtime_->view = new MarkdownView(this);
    runtime_->view->setMaxContentWidth(UiZoom::s(900));
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
    attachment->setAccessibleName(QStringLiteral("跨设备暂不支持附件"));
    attachment->setToolTip(attachment->accessibleName());
    attachment->setFixedSize(UiZoom::s(36), UiZoom::s(36));
    attachment->setStyleSheet(QStringLiteral(
        "QPushButton{border:0;border-radius:18px;background:#f1f5f9;"
        "color:#94a3b8;font-size:24px;}"));
    attachment->setEnabled(false);
    runtime_->editor->setLeadingAction(attachment);
    composerLayout->addWidget(runtime_->editor, 1);
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
    runtime_->refresh.setInterval(5000);
    connect(&runtime_->refresh, &QTimer::timeout, this, [this] {
        if (isVisible()) refreshHistory();
    });
    runtime_->historyTimeout.setSingleShot(true);
    runtime_->historyTimeout.setInterval(30'000);
    connect(&runtime_->historyTimeout, &QTimer::timeout, this, [this] {
        if (!runtime_->historyInFlight) return;
        runtime_->historyInFlight = false;
        runtime_->older->setEnabled(true);
        if (runtime_->messages.isEmpty()) {
            runtime_->refresh.stop();
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
        runtime_->pendingAction.clear();
        updateThinking();
        if (runtime_->editor->toPlainText().isEmpty() && !runtime_->pendingDraft.isEmpty())
            runtime_->editor->setPlainText(runtime_->pendingDraft);
        runtime_->pendingDraft.clear();
        showStatus(QStringLiteral("是否执行成功尚未确定，请先查看所属设备的会话记录，不要直接重复发送"), true);
        updateControls();
    });
    updateControls();
}

RemoteAgentPanel::~RemoteAgentPanel() = default;

void RemoteAgentPanel::setCommandHandler(CommandHandler handler) { runtime_->command = std::move(handler); }
void RemoteAgentPanel::setHistoryHandler(HistoryHandler handler) { runtime_->history = std::move(handler); }

QJsonObject RemoteAgentPanel::session() const { return runtime_->session; }

void RemoteAgentPanel::openSession(const QJsonObject& remote) {
    const QString previous = sessionKey(runtime_->session);
    const QString next = sessionKey(remote);
    if (previous != next) {
        ++runtime_->historyGeneration;
        runtime_->historyInFlight = false;
        runtime_->historyTimeout.stop();
        if (!previous.isEmpty()) runtime_->drafts.insert(previous, runtime_->editor->toPlainText());
        runtime_->editor->setPlainText(runtime_->drafts.value(next));
        runtime_->messages.clear();
        runtime_->rendered.clear();
        runtime_->view->clear();
        runtime_->thinking = nullptr;
        runtime_->oldest.clear();
        runtime_->older->hide();
        runtime_->pendingCommand = false;
        runtime_->lastCommandRejected = false;
        runtime_->pendingDraft.clear();
        runtime_->awaitingResponse = false;
        runtime_->observedBusy = false;
        runtime_->pendingAction.clear();
        runtime_->commandTimeout.stop();
    }
    runtime_->session = remote;
    runtime_->refresh.start();
    updateControls();
    if (previous == next && !runtime_->messages.isEmpty()) return;
    showStatus(remote.value(QStringLiteral("online")).toBool()
                   ? QStringLiteral("正在读取所属设备的会话…")
                   : QStringLiteral("所属设备离线；输入保留为草稿"));
    requestHistory({}, 10);
}

void RemoteAgentPanel::updateSession(const QJsonObject& remote) {
    if (sessionKey(remote) != sessionKey(runtime_->session)) return;
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
    }
}

void RemoteAgentPanel::refreshHistory() {
    requestHistory({}, 1);
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
    runtime_->lastCommandRejected = false;
    runtime_->pendingAction = action.value(QStringLiteral("action")).toString();
    runtime_->commandSentAt = QDateTime::currentMSecsSinceEpoch();
    if (runtime_->pendingAction != QStringLiteral("prompt")) runtime_->awaitingResponse = false;
    runtime_->pendingDraft = clearDraft ? runtime_->editor->toPlainText() : QString();
    updateControls();
    runtime_->command(current, action, [this, current, clearDraft](const QString& error) {
        if (sessionKey(current) != sessionKey(runtime_->session)) return;
        if (!error.isEmpty()) {
            runtime_->pendingCommand = false;
            runtime_->pendingDraft.clear();
            runtime_->pendingAction.clear();
            runtime_->awaitingResponse = false;
            updateThinking();
            showStatus(error, true);
            updateControls();
            return;
        }
        if (!runtime_->pendingCommand) return;
        if (clearDraft && !runtime_->lastCommandRejected) runtime_->editor->clear();
        showStatus(QStringLiteral("已送达所属设备，等待确认执行"));
        runtime_->commandTimeout.start();
    });
}

void RemoteAgentPanel::sendOrStop() {
    if (runtime_->session.value(QStringLiteral("busy")).toBool()) {
        sendAction({{QStringLiteral("action"), QStringLiteral("stop")}}, false);
        return;
    }
    const QString text = runtime_->editor->toPlainText().trimmed();
    if (text.isEmpty() || text.toUtf8().size() > 16 * 1024) return;
    sendAction({{QStringLiteral("action"), QStringLiteral("prompt")}, {QStringLiteral("text"), text}}, true);
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
        if (!runtime_->lastCommandRejected && !runtime_->pendingDraft.isEmpty() &&
            runtime_->editor->toPlainText() == runtime_->pendingDraft)
            runtime_->editor->clear();
        if (runtime_->lastCommandRejected && runtime_->editor->toPlainText().isEmpty() &&
            !runtime_->pendingDraft.isEmpty())
            runtime_->editor->setPlainText(runtime_->pendingDraft);
        runtime_->pendingDraft.clear();
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
        addDecision(QStringLiteral("允许一次"), QStringLiteral("approve_once"));
        if (permission.value(QStringLiteral("allowForSession")).toBool())
            addDecision(QStringLiteral("本次会话允许"), QStringLiteral("approve_session"));
        addDecision(QStringLiteral("拒绝"), QStringLiteral("deny"));
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
    runtime_->device->setText(QStringLiteral("%1 · %2")
        .arg(runtime_->session.value(QStringLiteral("device_name")).toString(),
             online ? QStringLiteral("在线") : QStringLiteral("离线")));
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
                               (busy || !runtime_->editor->toPlainText().trimmed().isEmpty()));
}

void RemoteAgentPanel::showStatus(const QString& message, bool failure) {
    runtime_->status->setText(message);
    runtime_->status->setVisible(!message.isEmpty());
    runtime_->status->setStyleSheet(failure ? QStringLiteral("color:#b42318;background:#fff5f3;")
                                            : QStringLiteral("color:#667085;background:transparent;font-size:12px;"));
}
