// 文件作用：呈现远端 Session 的可见记录；输入和权限决定都发往所属设备。
#include "agent/RemoteAgentPanel.h"

#include <QDateTime>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QPushButton>
#include <QTextEdit>
#include <QTimer>
#include <QVBoxLayout>

#include "markdown/MarkdownView.h"
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

}  // namespace

struct RemoteAgentPanel::Runtime {
    QJsonObject session;
    QLabel* device = nullptr;
    QLabel* status = nullptr;
    MarkdownView* view = nullptr;
    QTextEdit* editor = nullptr;
    QPushButton* send = nullptr;
    QPushButton* older = nullptr;
    QWidget* requests = nullptr;
    QVBoxLayout* requestsLayout = nullptr;
    QTimer refresh;
    QTimer commandTimeout;
    QMap<QString, QJsonObject> messages;
    QMap<QString, QString> rendered;
    QHash<QString, QString> drafts;
    QString oldest;
    bool pendingCommand = false;
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
    runtime_->status->setWordWrap(true);
    runtime_->status->setContentsMargins(UiZoom::s(16), UiZoom::s(6), UiZoom::s(16), UiZoom::s(6));
    runtime_->status->hide();
    layout->addWidget(runtime_->status);
    auto* composer = new QWidget(this);
    auto* composerLayout = new QHBoxLayout(composer);
    composerLayout->setContentsMargins(UiZoom::s(16), UiZoom::s(10), UiZoom::s(16), UiZoom::s(10));
    runtime_->editor = new QTextEdit(composer);
    runtime_->editor->setObjectName(QStringLiteral("remoteAgentComposer"));
    runtime_->editor->setAcceptRichText(false);
    runtime_->editor->setPlaceholderText(QStringLiteral("向所属设备发送消息…"));
    runtime_->editor->setFixedHeight(UiZoom::s(100));
    runtime_->editor->setStyleSheet(QStringLiteral(
        "QTextEdit{border:1px solid #cbd5e1;border-radius:10px;padding:10px;color:#172033;}"));
    composerLayout->addWidget(runtime_->editor, 1);
    runtime_->send = new QPushButton(QStringLiteral("发送"), composer);
    runtime_->send->setObjectName(QStringLiteral("remoteAgentSendButton"));
    runtime_->send->setFixedSize(UiZoom::s(64), UiZoom::s(40));
    runtime_->send->setStyleSheet(QStringLiteral(
        "QPushButton{background:#0b67b7;color:white;border-radius:8px;}"
        "QPushButton:disabled{background:#e2e8f0;color:#98a2b3;}"));
    composerLayout->addWidget(runtime_->send, 0, Qt::AlignBottom);
    layout->addWidget(composer);
    connect(runtime_->send, &QPushButton::clicked, this, [this] { sendOrStop(); });
    connect(runtime_->editor, &QTextEdit::textChanged, this, [this] { updateControls(); });
    connect(runtime_->older, &QPushButton::clicked, this, [this] {
        if (!runtime_->history || runtime_->oldest.isEmpty()) return;
        const QJsonObject current = runtime_->session;
        runtime_->history(current, runtime_->oldest, 20, [this, current](const QString& error) {
            if (!error.isEmpty() && sessionKey(current) == sessionKey(runtime_->session)) showStatus(error, true);
        });
    });
    runtime_->refresh.setInterval(5000);
    connect(&runtime_->refresh, &QTimer::timeout, this, [this] { refreshHistory(); });
    runtime_->commandTimeout.setSingleShot(true);
    runtime_->commandTimeout.setInterval(30'000);
    connect(&runtime_->commandTimeout, &QTimer::timeout, this, [this] {
        if (!runtime_->pendingCommand) return;
        runtime_->pendingCommand = false;
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
        if (!previous.isEmpty()) runtime_->drafts.insert(previous, runtime_->editor->toPlainText());
        runtime_->editor->setPlainText(runtime_->drafts.value(next));
        runtime_->messages.clear();
        runtime_->rendered.clear();
        runtime_->view->clear();
        runtime_->oldest.clear();
        runtime_->older->hide();
        runtime_->pendingCommand = false;
        runtime_->commandTimeout.stop();
    }
    runtime_->session = remote;
    runtime_->refresh.start();
    updateControls();
    showStatus(remote.value(QStringLiteral("online")).toBool()
                   ? QStringLiteral("正在读取所属设备的会话…")
                   : QStringLiteral("所属设备离线；输入保留为草稿"));
    refreshHistory();
}

void RemoteAgentPanel::updateSession(const QJsonObject& remote) {
    if (sessionKey(remote) != sessionKey(runtime_->session)) return;
    runtime_->session = remote;
    updateControls();
    if (!remote.value(QStringLiteral("online")).toBool())
        showStatus(QStringLiteral("所属设备离线；输入保留为草稿"));
}

void RemoteAgentPanel::refreshHistory() {
    if (!runtime_->history || !runtime_->session.value(QStringLiteral("online")).toBool()) return;
    const QJsonObject current = runtime_->session;
    runtime_->history(current, {}, 10, [this, current](const QString& error) {
        if (!error.isEmpty() && sessionKey(current) == sessionKey(runtime_->session)) showStatus(error, true);
    });
}

void RemoteAgentPanel::sendAction(const QJsonObject& action, bool clearDraft) {
    if (!runtime_->command || runtime_->pendingCommand) return;
    const QJsonObject current = runtime_->session;
    runtime_->pendingCommand = true;
    updateControls();
    runtime_->command(current, action, [this, current, clearDraft](const QString& error) {
        if (sessionKey(current) != sessionKey(runtime_->session)) return;
        if (!error.isEmpty()) {
            runtime_->pendingCommand = false;
            showStatus(error, true);
            updateControls();
            return;
        }
        if (clearDraft) runtime_->editor->clear();
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
        showStatus(result.value(QStringLiteral("ok")).toBool()
                       ? QStringLiteral("所属设备已接受操作")
                       : result.value(QStringLiteral("error")).toString(),
                   !result.value(QStringLiteral("ok")).toBool());
        updateControls();
        return;
    }
    if (!payload.value(QStringLiteral("ok")).toBool()) return;
    const QJsonArray received = payload.value(QStringLiteral("messages")).toArray();
    for (const QJsonValue& value : received) {
        const QJsonObject message = value.toObject();
        const QString id = message.value(QStringLiteral("id")).toString();
        if (!id.isEmpty()) runtime_->messages.insert(id, message);
    }
    if (!received.isEmpty() && kind == QStringLiteral("history_page")) {
        const QString oldest = received.last().toObject().value(QStringLiteral("id")).toString();
        if (runtime_->oldest.isEmpty() || oldest < runtime_->oldest) runtime_->oldest = oldest;
        runtime_->older->setVisible(received.size() >= 10);
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
    if (!prepend && followedLatest) runtime_->view->scrollToBottom();
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
    runtime_->send->setText(busy ? QStringLiteral("停止") : QStringLiteral("发送"));
    runtime_->send->setEnabled(online && !runtime_->pendingCommand &&
                               (busy || !runtime_->editor->toPlainText().trimmed().isEmpty()));
}

void RemoteAgentPanel::showStatus(const QString& message, bool failure) {
    runtime_->status->setText(message);
    runtime_->status->setVisible(!message.isEmpty());
    runtime_->status->setStyleSheet(failure ? QStringLiteral("color:#b42318;background:#fff5f3;")
                                            : QStringLiteral("color:#0b67b7;background:#edf8ff;"));
}
