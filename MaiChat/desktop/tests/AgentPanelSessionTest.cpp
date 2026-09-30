#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QLabel>
#include <QMenu>
#include <QMimeData>
#include <QPushButton>
#include <QSemaphore>
#include <QScrollBar>
#include <QSignalSpy>
#include <QSplitter>
#include <QSplitterHandle>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextEdit>
#include <QUrl>
#include <QtTest>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "agent/AgentChatPanel.h"
#include "agent/AgentController.h"
#include "markdown/MarkdownView.h"
#include "ui/ComposerTextEdit.h"
#include "ui/UiZoom.h"

// AI 面板：**别的会话说的话不能灌进这个会话的界面**。
//
// 子 Agent 是并发跑在另一个会话里的。面板以前每个事件处理都忽略 sessionId——
// 那时候只有一个会话在跑，无害；子 Agent 一来就不是了：
//
//   正文   子的回答会接进父的答案里，用户看到「AI 说了些我没问的话」
//   状态   子跑完会把发送按钮从「停止」翻回「发送」，而父还在跑
//   标题   子的任务名会顶掉头部那一行
//
// 授权和提问是**例外**，那两样刻意不过滤：子 Agent 没有自己的界面，
// 滤掉的话它会永远挂在闸门上等一个不会来的点头。

namespace {

// 一个什么都不说的模型。这份用例不需要真跑起来的回答——
// 事件是直接往总线上发的，那才是要测的路径。
class SilentModel final : public MaiModelClient {
public:
    MaiError stream(const MaiModelRequest& request, const MaiStreamSink& sink,
                    const std::atomic<bool>& cancel) override {
        (void)request;
        (void)sink;
        (void)cancel;
        return {};
    }
    MaiWireApi wireApi() const override {
        return MaiWireApi::ChatCompletions;
    }
};

class CapturingModel final : public MaiModelClient {
public:
    MaiError stream(const MaiModelRequest& request, const MaiStreamSink& sink,
                    const std::atomic<bool>& cancel) override {
        (void)cancel;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            request_ = request;
        }
        if (sink.onText) sink.onText("done");
        return {};
    }
    MaiWireApi wireApi() const override { return MaiWireApi::ChatCompletions; }
    MaiModelRequest request() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return request_;
    }

private:
    mutable std::mutex mutex_;
    MaiModelRequest request_;
};

class PdfGeneratingModel final : public MaiModelClient {
public:
    MaiError stream(const MaiModelRequest&, const MaiStreamSink& sink,
                    const std::atomic<bool>&) override {
        if (calls_++ == 0) {
            if (sink.onToolCall) sink.onToolCall(MaiToolInvocation{
                "pdf-call", "generate_pdf",
                R"({"content":"# Preview test","output_path":"preview.pdf"})"});
        } else if (sink.onText) {
            sink.onText("PDF created");
        }
        return {};
    }
    MaiWireApi wireApi() const override { return MaiWireApi::ChatCompletions; }

private:
    std::atomic<int> calls_{0};
};

class DelayedFirstChunkModel final : public MaiModelClient {
public:
    explicit DelayedFirstChunkModel(bool reasoning, int warmupCalls = 0)
        : reasoning_(reasoning), warmupCalls_(warmupCalls) {}

    MaiError stream(const MaiModelRequest&, const MaiStreamSink& sink,
                    const std::atomic<bool>& cancel) override {
        if (calls_.fetch_add(1) < warmupCalls_) {
            std::string answer;
            for (int i = 0; i < 500; ++i) answer += "previous answer ";
            if (sink.onText) sink.onText(answer);
            return {};
        }
        entered_.store(true);
        gate_.tryAcquire(1, 3000);
        if (cancel.load()) return {};
        if (reasoning_ && sink.onReasoning) sink.onReasoning("Thinking");
        if (sink.onText) sink.onText("Answer");
        return {};
    }

    MaiWireApi wireApi() const override { return MaiWireApi::ChatCompletions; }
    void release() { gate_.release(); }
    bool entered() const { return entered_.load(); }

private:
    QSemaphore gate_;
    std::atomic<bool> entered_{false};
    std::atomic<int> calls_{0};
    bool reasoning_ = false;
    int warmupCalls_ = 0;
};

class ApprovalModel final : public MaiModelClient {
public:
    MaiError stream(const MaiModelRequest&, const MaiStreamSink& sink,
                    const std::atomic<bool>&) override {
        if (calls_++ == 0) {
            if (sink.onToolCall)
                sink.onToolCall(MaiToolInvocation{
                    "call-send", "maichat_send_text",
                    R"({"peer_id":"house-multi-ai-code","text":"hello"})"});
        } else if (sink.onText) {
            sink.onText("done");
        }
        return {};
    }

    MaiWireApi wireApi() const override { return MaiWireApi::ChatCompletions; }

private:
    std::atomic<int> calls_{0};
};

class ApprovalTool final : public MaiTool {
public:
    explicit ApprovalTool(std::atomic<int>& executed) : executed_(executed) {}
    std::string name() const override { return "maichat_send_text"; }
    std::string description() const override { return "Send a test message."; }
    std::string parametersSchema() const override { return R"({"type":"object"})"; }
    bool requiresApproval(const std::string&) const override { return true; }
    bool requiresPerCallApproval(const std::string&) const override { return true; }
    MaiToolResult execute(const std::string&, const MaiToolContext&) override {
        ++executed_;
        return MaiToolResult::success("queued");
    }

private:
    std::atomic<int>& executed_;
};

class QuestionModel final : public MaiModelClient {
public:
    MaiError stream(const MaiModelRequest&, const MaiStreamSink& sink,
                    const std::atomic<bool>&) override {
        if (calls_++ == 0) {
            if (sink.onToolCall)
                sink.onToolCall(MaiToolInvocation{
                    "call-question", "question", R"({"question":"Choose A or B"})"});
        } else if (sink.onText) {
            sink.onText("answered");
        }
        return {};
    }

    MaiWireApi wireApi() const override { return MaiWireApi::ChatCompletions; }

private:
    std::atomic<int> calls_{0};
};

class PartialReasoningModel final : public MaiModelClient {
public:
    std::atomic<bool> release{false};

    MaiError stream(const MaiModelRequest&, const MaiStreamSink& sink,
                    const std::atomic<bool>& cancel) override {
        if (sink.onReasoning) sink.onReasoning("partial reasoning");
        while (!release.load() && !cancel.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (sink.onReasoning && !cancel.load()) sink.onReasoning(" completed");
        return {};
    }

    MaiWireApi wireApi() const override { return MaiWireApi::ChatCompletions; }
};

}  // namespace

class AgentPanelSessionTest : public QObject {
    Q_OBJECT

private slots:
    void newSessionAvoidsFilesystemRoot();
    void newSessionUsesDocumentsInsteadOfProcessDirectory();
    void newSessionOffersWorkspaceChoice();
    void chosenWorkspaceCreatesIndependentSession();
    void answerFromAnotherSessionDoesNotLeakIn();
    void anotherSessionFailingDoesNotShowAnError();
    void anotherSessionTitleDoesNotRenameTheHeader();
    void approvalFromAnotherSessionIsStillShown();
    void pendingApprovalIsRestoredAfterSessionSwitch();
    void pendingQuestionIsRestoredOnlyInItsSession();
    void composerDraftsStayInTheirSessions();
    void waitingIndicatorSurvivesSwitchingAwayAndBack();
    void partialReasoningSurvivesSwitchBeforeCheckpoint();
    void thinkingLineExpandsLiveAndAfterRestore();
    void waitingIndicatorAppearsBeforeFirstModelChunk();
    void waitingIndicatorClearsWhenModelHasNoReasoning();
    void unconfiguredModelOpensConfigurationInsteadOfFailingTurn();
    void desktopAgentSuppliesMarkdownSystemPrompt();
    void composerMatchesImLayoutAndUsesEmbeddedSendAction();
    void conversationAndComposerUseResizableSplitter();
    void composerPlaceholderFitsAtMinimumHeight();
    void conversationUsesAvailableWidth();
    void pastedImageUsesTheSharedComposerAndReachesTheModel();
    void modelChipOffersTheTextAndVisionModels();
    void newAgentOutputDoesNotInterruptReadingHistory();
    void completedPdfToolShowsPreviewCardAfterRestore();
};

namespace {

// 一套跑起来的面板 + 它背后的核心。
struct Harness {
    std::unique_ptr<AgentController> controller;
    std::unique_ptr<AgentChatPanel> panel;
    QString mine;
    QString other;

    Harness() {
        // 空的数据库路径 = 内存库，用例之间互不影响。
        controller = std::make_unique<AgentController>(std::make_unique<SilentModel>(), QString());
        panel = std::make_unique<AgentChatPanel>(*controller);
        panel->resize(700, 500);
        panel->show();
        mine = controller->createSession(QDir::currentPath());
        other = controller->createSession(QDir::currentPath());
        panel->openSession(mine);
    }

    MarkdownView* view() const {
        return panel->findChild<MarkdownView*>();
    }

    QPushButton* sendButton() const {
        return panel->findChild<QPushButton*>(QStringLiteral("agentSendButton"));
    }

    // 直接往事件总线上发一条，模拟另一个会话（子 Agent）在动。
    void publishFromOther(MaiEventType type, const QString& detail = QString()) {
        MaiEvent event;
        event.id = "evt_test";
        event.type = type;
        event.sessionId = other.toUtf8().constData();
        event.messageId = "msg_other";
        event.partId = "prt_other";
        event.field = "text";
        event.delta = detail.toUtf8().constData();
        event.detail = detail.toUtf8().constData();
        controller->agent().eventBus().publish(event);
        // 事件是排队送到主线程的，等它到。
        QTest::qWait(120);
    }
};

}  // namespace

void AgentPanelSessionTest::completedPdfToolShowsPreviewCardAfterRestore() {
    QTemporaryDir workspace;
    QVERIFY(workspace.isValid());
    AgentController controller(std::make_unique<PdfGeneratingModel>(), QString());
    AgentChatPanel panel(controller);
    panel.resize(700, 500);
    panel.show();
    const QString session = controller.createSession(workspace.path());
    panel.openSession(session);
    QVERIFY(controller.sendPrompt(session, QStringLiteral("Create a PDF")));
    QTRY_VERIFY_WITH_TIMEOUT(!controller.agent().listPendingPermissions().empty(), 5000);
    const auto pending = controller.agent().listPendingPermissions();
    QVERIFY(controller.approvePermission(QString::fromStdString(pending.front().id)));
    controller.agent().waitIdle();
    QTRY_VERIFY_WITH_TIMEOUT(
        panel.findChild<QPushButton*>(QStringLiteral("agentPdfPreviewButton")) != nullptr,
        5000);
    QVERIFY(QFile::exists(workspace.filePath(QStringLiteral("preview.pdf"))));

    panel.openSession(session);
    QTRY_VERIFY_WITH_TIMEOUT(
        panel.findChild<QPushButton*>(QStringLiteral("agentPdfPreviewButton")) != nullptr,
        5000);
}

void AgentPanelSessionTest::newSessionAvoidsFilesystemRoot() {
    struct RestoreCurrentDirectory {
        QString original = QDir::currentPath();
        ~RestoreCurrentDirectory() { QDir::setCurrent(original); }
    } restore;
    QVERIFY(QDir::setCurrent(QDir::rootPath()));

    AgentController controller(std::make_unique<SilentModel>(), QString());
    AgentChatPanel panel(controller);
    panel.openSession(QString());
    const auto sessions = controller.agent().listSessions();
    QCOMPARE(sessions.size(), std::size_t(1));
    QVERIFY(QString::fromStdString(sessions.front().directory) != QDir::rootPath());
}

void AgentPanelSessionTest::newAgentOutputDoesNotInterruptReadingHistory() {
    Harness harness;
    QVERIFY(QTest::qWaitForWindowExposed(harness.panel.get()));
    auto* view = harness.view();
    auto publish = [&](const char* partId, const QString& text) {
        MaiEvent event;
        event.id = "evt_scroll_test";
        event.type = MaiEventType::MessagePartDelta;
        event.sessionId = harness.mine.toUtf8().constData();
        event.messageId = "msg_scroll_test";
        event.partId = partId;
        event.field = "text";
        event.delta = text.toUtf8().constData();
        harness.controller->agent().eventBus().publish(event);
        QTest::qWait(120);
    };
    publish("part_history", QStringLiteral("history line\n").repeated(250));
    auto* bar = view->verticalScrollBar();
    QVERIFY(bar->maximum() > 0);
    bar->setValue(qMax(0, bar->maximum() / 3));
    QVERIFY(!view->isAtBottom());
    const int position = bar->value();

    publish("part_new", QStringLiteral("A new answer arrived while reading history"));
    QCOMPARE(bar->value(), position);
    publish("part_new", QStringLiteral(" and it kept streaming"));
    QCOMPARE(bar->value(), position);
}

void AgentPanelSessionTest::answerFromAnotherSessionDoesNotLeakIn() {
    Harness harness;
    QVERIFY(QTest::qWaitForWindowExposed(harness.panel.get()));
    MarkdownView* view = harness.view();
    QVERIFY(view != nullptr);
    const int before = view->itemCount();

    harness.publishFromOther(MaiEventType::MessagePartDelta,
                             QStringLiteral("这是子 Agent 说的话"));

    // 一条都不该多出来。多出来的话用户会以为 AI 自己说了些没问过的东西。
    QCOMPARE(view->itemCount(), before);
    view->selectAll();
    QVERIFY(!view->selectedText().contains(QStringLiteral("子 Agent 说的话")));
}

void AgentPanelSessionTest::modelChipOffersTheTextAndVisionModels() {
    Harness harness;
    harness.panel->setModelLabel(QStringLiteral("glm-5.3"));
    QPushButton* chip = harness.panel->findChild<QPushButton*>(QStringLiteral("agentModelChip"));
    QVERIFY(chip != nullptr);
    QVERIFY(chip->menu() != nullptr);
    QCOMPARE(chip->menu()->actions().size(), 2);
    QCOMPARE(chip->menu()->actions()[0]->text(), QStringLiteral("glm-5.3"));
    QCOMPARE(chip->menu()->actions()[1]->text(), QStringLiteral("glm-5.3-flash"));

    QSignalSpy selected(harness.panel.get(), &AgentChatPanel::modelSelected);
    chip->menu()->actions()[1]->trigger();
    QCOMPARE(selected.count(), 1);
    QCOMPARE(selected.first().first().toString(), QStringLiteral("glm-5.3-flash"));
    MaiSession session;
    QVERIFY(harness.controller->agent().getSession(harness.mine.toStdString(), session));
    QCOMPARE(QString::fromStdString(session.model), QStringLiteral("glm-5.3-flash"));
}

void AgentPanelSessionTest::anotherSessionFailingDoesNotShowAnError() {
    // 子 Agent 跑挂了是**它自己的事**：父那边会从 wait_agent 拿到结果，
    // 该怎么应对由模型决定。直接在父的界面上弹一条红字的话，用户会以为
    // 自己刚才那句话出错了。
    Harness harness;
    QVERIFY(QTest::qWaitForWindowExposed(harness.panel.get()));
    MarkdownView* view = harness.view();
    QVERIFY(view != nullptr);
    const int before = view->itemCount();

    harness.publishFromOther(MaiEventType::SessionError, QStringLiteral("子 Agent 炸了"));

    QCOMPARE(view->itemCount(), before);
    view->selectAll();
    QVERIFY(!view->selectedText().contains(QStringLiteral("炸了")));
}

void AgentPanelSessionTest::anotherSessionTitleDoesNotRenameTheHeader() {
    Harness harness;
    QVERIFY(QTest::qWaitForWindowExposed(harness.panel.get()));

    QLabel* title = nullptr;
    for (QLabel* label : harness.panel->findChildren<QLabel*>()) {
        if (label->text() == QStringLiteral("AI 助手")) title = label;
    }
    QVERIFY(title != nullptr);

    harness.publishFromOther(MaiEventType::SessionUpdated, QStringLiteral("子任务的名字"));
    QCOMPARE(title->text(), QStringLiteral("AI 助手"));
}

void AgentPanelSessionTest::approvalFromAnotherSessionIsStillShown() {
    // **这条和上面几条方向相反，是故意的。**
    //
    // 子 Agent 没有自己的界面。它要审批时如果被过滤掉，它会永远挂在闸门上，
    // 而用户只看到「一直在跑」。所以授权和提问**不按会话过滤**。
    Harness harness;
    QVERIFY(QTest::qWaitForWindowExposed(harness.panel.get()));

    QSignalSpy spy(harness.controller.get(), &AgentController::permissionAsked);
    MaiEvent asked;
    asked.id = "evt_perm";
    asked.type = MaiEventType::PermissionAsked;
    asked.sessionId = harness.other.toUtf8().constData();
    asked.messageId = "msg_other";
    asked.partId = "prt_other";
    asked.permissionId = "per_other";
    harness.controller->agent().eventBus().publish(asked);
    QTest::qWait(120);

    // 信号要到得了面板。面板拿这个 id 去核心查待裁决列表（那份列表是全局的，
    // 不分会话），所以子 Agent 的授权照样答得了。
    QCOMPARE(spy.count(), 1);
}

void AgentPanelSessionTest::newSessionUsesDocumentsInsteadOfProcessDirectory() {
    QTemporaryDir processDirectory;
    QVERIFY(processDirectory.isValid());
    struct RestoreCurrentDirectory {
        QString original = QDir::currentPath();
        ~RestoreCurrentDirectory() { QDir::setCurrent(original); }
    } restore;
    QVERIFY(QDir::setCurrent(processDirectory.path()));

    AgentController controller(std::make_unique<SilentModel>(), QString());
    AgentChatPanel panel(controller);
    panel.openSession();
    const auto sessions = controller.agent().listSessions();
    QCOMPARE(sessions.size(), std::size_t(1));
    QString documents = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    if (documents.isEmpty() || !QFileInfo(documents).isDir() ||
        !QFileInfo(documents).isWritable()) documents = QDir::homePath();
    QCOMPARE(QDir(QString::fromStdString(sessions.front().directory)).canonicalPath(),
             QDir(documents).canonicalPath());
}

void AgentPanelSessionTest::newSessionOffersWorkspaceChoice() {
    Harness harness;
    auto* more = harness.panel->findChild<QPushButton*>(QStringLiteral("agentMoreActions"));
    QVERIFY(more != nullptr);
    QVERIFY(more->menu() != nullptr);
    bool hasChoice = false;
    for (QAction* action : more->menu()->actions()) {
        if (action->text() == QStringLiteral("选择工作目录并新建对话…")) hasChoice = true;
    }
    QVERIFY(hasChoice);
}

void AgentPanelSessionTest::chosenWorkspaceCreatesIndependentSession() {
    QTemporaryDir firstDirectory;
    QTemporaryDir secondDirectory;
    QVERIFY(firstDirectory.isValid());
    QVERIFY(secondDirectory.isValid());
    AgentController controller(std::make_unique<SilentModel>(), QString());
    AgentChatPanel panel(controller);

    panel.openSessionInDirectory(firstDirectory.path());
    const QString firstSession = panel.sessionId();
    QVERIFY(!firstSession.isEmpty());
    panel.openSessionInDirectory(secondDirectory.path());
    const QString secondSession = panel.sessionId();
    QVERIFY(!secondSession.isEmpty());
    QVERIFY(secondSession != firstSession);

    MaiSession first;
    MaiSession second;
    QVERIFY(controller.agent().getSession(firstSession.toStdString(), first));
    QVERIFY(controller.agent().getSession(secondSession.toStdString(), second));
    QCOMPARE(QDir(QString::fromStdString(first.directory)).canonicalPath(),
             QDir(firstDirectory.path()).canonicalPath());
    QCOMPARE(QDir(QString::fromStdString(second.directory)).canonicalPath(),
             QDir(secondDirectory.path()).canonicalPath());
}

void AgentPanelSessionTest::pendingApprovalIsRestoredAfterSessionSwitch() {
    std::atomic<int> executed{0};
    AgentController controller(
        std::make_unique<ApprovalModel>(), QString(),
        [&executed](MaiToolRegistry& tools) {
            tools.add(std::make_unique<ApprovalTool>(executed));
        });
    AgentChatPanel panel(controller);
    panel.resize(800, 550);
    panel.show();
    QVERIFY(QTest::qWaitForWindowExposed(&panel));
    const QString session = controller.createSession(QDir::currentPath());
    panel.openSession(session);
    auto* editor = panel.findChild<QTextEdit*>();
    auto* send = panel.findChild<QPushButton*>(QStringLiteral("agentSendButton"));
    QVERIFY(editor != nullptr);
    QVERIFY(send != nullptr);
    editor->setPlainText(QStringLiteral("send the report"));
    send->click();
    QTRY_COMPARE(controller.agent().listPendingPermissions().size(), std::size_t(1));
    QCOMPARE(send->toolTip(), QStringLiteral("停止任务"));

    auto approvalIsVisible = [&panel] {
        for (QLabel* label : panel.findChildren<QLabel*>()) {
            if (label->text() == QStringLiteral("等你点头") && label->isVisible()) return true;
        }
        return false;
    };
    QTRY_VERIFY(approvalIsVisible());

    panel.openSession(controller.createSession(QDir::currentPath()));
    QCOMPARE(send->toolTip(), QStringLiteral("发送消息"));
    auto unrelatedApprovalIsVisible = [&panel] {
        for (QWidget* card : panel.findChildren<QWidget*>(QStringLiteral("agentToolCard"))) {
            if (card->isVisible()) return true;
        }
        return false;
    };
    QTRY_VERIFY(!unrelatedApprovalIsVisible());
    panel.openSession(session);
    QCOMPARE(send->toolTip(), QStringLiteral("停止任务"));
    QTRY_VERIFY2(approvalIsVisible(), "A pending send must not become an unapprovable queue item");
    QPushButton* allow = nullptr;
    for (QPushButton* button : panel.findChildren<QPushButton*>()) {
        if (button->text() == QStringLiteral("允许") && button->isVisible()) allow = button;
    }
    QVERIFY(allow != nullptr);
    allow->click();
    controller.agent().waitIdle();
    QCOMPARE(executed.load(), 1);
}

void AgentPanelSessionTest::pendingQuestionIsRestoredOnlyInItsSession() {
    AgentController controller(std::make_unique<QuestionModel>(), QString());
    AgentChatPanel panel(controller);
    panel.resize(800, 550);
    panel.show();
    QVERIFY(QTest::qWaitForWindowExposed(&panel));
    const QString session = controller.createSession(QDir::currentPath());
    panel.openSession(session);
    auto* editor = panel.findChild<QTextEdit*>();
    auto* send = panel.findChild<QPushButton*>(QStringLiteral("agentSendButton"));
    QVERIFY(editor != nullptr);
    QVERIFY(send != nullptr);
    editor->setPlainText(QStringLiteral("ask me"));
    send->click();
    QTRY_COMPARE(controller.pendingQuestions().size(), std::size_t(1));
    QTRY_COMPARE(editor->placeholderText(), QStringLiteral("回答它…"));

    panel.openSession(controller.createSession(QDir::currentPath()));
    QCOMPARE(editor->placeholderText(), QStringLiteral("交给它做点什么…"));
    QCOMPARE(send->toolTip(), QStringLiteral("发送消息"));
    panel.openSession(session);
    QCOMPARE(editor->placeholderText(), QStringLiteral("回答它…"));
    editor->setPlainText(QStringLiteral("A"));
    send->click();
    controller.agent().waitIdle();
    QTRY_VERIFY(controller.pendingQuestions().empty());
}

void AgentPanelSessionTest::composerDraftsStayInTheirSessions() {
    Harness harness;
    QVERIFY(QTest::qWaitForWindowExposed(harness.panel.get()));
    auto* editor = harness.panel->findChild<QTextEdit*>();
    QVERIFY(editor != nullptr);
    editor->setPlainText(QStringLiteral("unsent in first session"));
    harness.panel->openSession(harness.other);
    QCOMPARE(editor->toPlainText(), QString());
    editor->setPlainText(QStringLiteral("unsent in second session"));
    harness.panel->openSession(harness.mine);
    QCOMPARE(editor->toPlainText(), QStringLiteral("unsent in first session"));
    harness.panel->openSession(harness.other);
    QCOMPARE(editor->toPlainText(), QStringLiteral("unsent in second session"));
}

void AgentPanelSessionTest::waitingIndicatorSurvivesSwitchingAwayAndBack() {
    auto model = std::make_unique<DelayedFirstChunkModel>(true);
    DelayedFirstChunkModel* delayed = model.get();
    AgentController controller(std::move(model), QString());
    AgentChatPanel panel(controller);
    panel.resize(700, 500);
    panel.show();
    QVERIFY(QTest::qWaitForWindowExposed(&panel));
    const QString session = controller.createSession(QDir::currentPath());
    panel.openSession(session);
    auto* editor = panel.findChild<QTextEdit*>();
    auto* send = panel.findChild<QPushButton*>(QStringLiteral("agentSendButton"));
    QVERIFY(editor != nullptr);
    QVERIFY(send != nullptr);
    editor->setPlainText(QStringLiteral("long-running request"));
    send->click();
    QTRY_VERIFY(delayed->entered());
    panel.openSession(controller.createSession(QDir::currentPath()));
    panel.openSession(session);
    auto hasVisibleThinking = [&panel] {
        for (QWidget* card : panel.findChildren<QWidget*>(QStringLiteral("agentThinkingCard"))) {
            if (card->isVisible()) return true;
        }
        return false;
    };
    QVERIFY2(hasVisibleThinking(), "The active session must show its waiting state immediately");
    delayed->release();
    controller.agent().waitIdle();
}

void AgentPanelSessionTest::partialReasoningSurvivesSwitchBeforeCheckpoint() {
    auto model = std::make_unique<PartialReasoningModel>();
    PartialReasoningModel* controlled = model.get();
    AgentController controller(std::move(model), QString());
    AgentChatPanel panel(controller);
    panel.resize(700, 500);
    panel.show();
    QVERIFY(QTest::qWaitForWindowExposed(&panel));
    const QString session = controller.createSession(QDir::currentPath());
    panel.openSession(session);
    auto* editor = panel.findChild<QTextEdit*>();
    auto* send = panel.findChild<QPushButton*>(QStringLiteral("agentSendButton"));
    QVERIFY(editor != nullptr);
    QVERIFY(send != nullptr);
    QSignalSpy reasoning(&controller, &AgentController::reasoningDelta);
    editor->setPlainText(QStringLiteral("inspect"));
    send->click();
    QTRY_VERIFY(reasoning.count() >= 1);

    panel.openSession(controller.createSession(QDir::currentPath()));
    panel.openSession(session);
    QWidget* card = nullptr;
    for (QWidget* item : panel.findChildren<QWidget*>(QStringLiteral("agentThinkingCard"))) {
        if (item->isVisible()) card = item;
    }
    QVERIFY(card != nullptr);
    QTest::mouseClick(card, Qt::LeftButton);
    auto* body = card->findChild<QLabel*>(QStringLiteral("agentThinkingBody"));
    QVERIFY(body != nullptr);
    QVERIFY2(body->text().contains(QStringLiteral("partial reasoning")),
             "Switching sessions lost reasoning already shown before the next disk checkpoint");
    controlled->release.store(true);
    controller.agent().waitIdle();
}

// 模型首包延迟时，发送后的等待状态仍要立即可见。
void AgentPanelSessionTest::waitingIndicatorAppearsBeforeFirstModelChunk() {
    auto model = std::make_unique<DelayedFirstChunkModel>(true, 1);
    DelayedFirstChunkModel* delayed = model.get();
    AgentController controller(std::move(model), QString());
    const QString session = controller.createSession(QDir::currentPath());
    QVERIFY(controller.sendPrompt(session, QStringLiteral("previous question")));
    controller.agent().waitIdle();
    AgentChatPanel panel(controller);
    panel.resize(700, 500);
    panel.show();
    QVERIFY(QTest::qWaitForWindowExposed(&panel));
    panel.openSession(session);

    auto* editor = panel.findChild<QTextEdit*>();
    auto* send = panel.findChild<QPushButton*>(QStringLiteral("agentSendButton"));
    QVERIFY(editor != nullptr);
    QVERIFY(send != nullptr);
    editor->setPlainText(QStringLiteral("你好"));
    QElapsedTimer timer;
    timer.start();
    QTest::mouseClick(send, Qt::LeftButton);
    qInfo() << "Agent prompt UI submit elapsed ms:" << timer.elapsed();

    QTRY_VERIFY(delayed->entered());
    qInfo() << "Agent request reached model stream after ms:" << timer.elapsed();
    auto* waiting = panel.findChild<QWidget*>(QStringLiteral("agentThinkingCard"));
    QVERIFY2(waiting != nullptr, "The waiting indicator must appear before the model sends data");
    QVERIFY(waiting->isVisible());

    delayed->release();
    QTRY_COMPARE(panel.findChildren<QWidget*>(QStringLiteral("agentThinkingCard")).size(), 1);
    QTRY_VERIFY(panel.findChild<QLabel*>(QStringLiteral("agentThinkingBody")) != nullptr);
    controller.agent().waitIdle();
}

void AgentPanelSessionTest::waitingIndicatorClearsWhenModelHasNoReasoning() {
    auto model = std::make_unique<DelayedFirstChunkModel>(false);
    DelayedFirstChunkModel* delayed = model.get();
    AgentController controller(std::move(model), QString());
    AgentChatPanel panel(controller);
    panel.resize(700, 500);
    panel.show();
    QVERIFY(QTest::qWaitForWindowExposed(&panel));
    panel.openSession(controller.createSession(QDir::currentPath()));

    auto* editor = panel.findChild<QTextEdit*>();
    auto* send = panel.findChild<QPushButton*>(QStringLiteral("agentSendButton"));
    QVERIFY(editor != nullptr);
    QVERIFY(send != nullptr);
    editor->setPlainText(QStringLiteral("你好"));
    QTest::mouseClick(send, Qt::LeftButton);
    QTRY_VERIFY(delayed->entered());
    QVERIFY(panel.findChild<QWidget*>(QStringLiteral("agentThinkingCard")) != nullptr);

    delayed->release();
    controller.agent().waitIdle();
    QTRY_VERIFY(panel.findChildren<QWidget*>(QStringLiteral("agentThinkingCard")).isEmpty());
    QVERIFY(panel.findChild<MarkdownView*>()->itemCount() >= 2);
}

// 思考条要能点开，而且**展开后高度必须真的长出来**：
// 之前出现过点击后正文露一条缝、卡片高度不动的情况（高度联动失效），
// 视觉上就是「点不开」或「文字被裁」。恢复路径（重开会话）同样要过一遍——
// 两条路建条目的方式不一样，坏一条不坏另一条是可能的。
void AgentPanelSessionTest::thinkingLineExpandsLiveAndAfterRestore() {
    // 会说话的假模型：一轮里给出一段够长的思考 + 一句正文。
    class ReasoningModel final : public MaiModelClient {
    public:
        MaiError stream(const MaiModelRequest& request, const MaiStreamSink& sink,
                        const std::atomic<bool>& cancel) override {
            (void)request;
            (void)cancel;
            if (sink.onReasoning) {
                for (int i = 0; i < 20; ++i) {
                    sink.onReasoning("想一想，这段思考要足够长，展开后的高度变化才量得出来。");
                }
            }
            if (sink.onText) sink.onText("回答完了。");
            return {};
        }
        MaiWireApi wireApi() const override { return MaiWireApi::ChatCompletions; }
    };

    AgentController controller(std::make_unique<ReasoningModel>(), QString());
    AgentChatPanel panel(controller);
    panel.resize(700, 500);
    panel.show();
    QVERIFY(QTest::qWaitForWindowExposed(&panel));
    const QString mine = controller.createSession(QDir::currentPath());
    panel.openSession(mine);

    auto* editor = panel.findChild<QTextEdit*>();
    QVERIFY(editor != nullptr);
    editor->setPlainText(QStringLiteral("问点什么"));
    auto* send =
        panel.findChild<QPushButton*>(QStringLiteral("agentSendButton"));
    QVERIFY(send != nullptr);
    QTest::mouseClick(send, Qt::LeftButton);

    // 等这一轮结束：思考条出现并塌成「已处理 N 秒」。
    QWidget* card = nullptr;
    QTRY_VERIFY((card = panel.findChild<QWidget*>(QStringLiteral("agentThinkingCard"))) != nullptr);
    QTRY_VERIFY(!card->findChildren<QLabel*>().isEmpty());

    auto bodyOf = [](QWidget* cardWidget) -> QLabel* {
        return cardWidget->findChild<QLabel*>(QStringLiteral("agentThinkingBody"));
    };

    QLabel* body = bodyOf(card);
    QVERIFY2(body != nullptr, "思考正文标签必须已经在条里");
    QTRY_VERIFY(!body->isVisible());
    QVERIFY2(body->text().isEmpty(), "收起状态不应反复排版完整思考正文");

    // ── 实时路径：点击展开，高度必须长出来 ──
    const int collapsedHeight = card->height();
    QTest::mouseClick(card, Qt::LeftButton, Qt::NoModifier, card->rect().center());
    QTRY_VERIFY2(body->isVisible(), "点击后思考正文必须显示出来");
    QVERIFY(body->text().contains(QStringLiteral("想一想")));
    QTRY_VERIFY2(card->height() > collapsedHeight + 20,
                 qPrintable(QStringLiteral("展开后卡片高度必须增长：收起 %1 → 展开 %2")
                                .arg(collapsedHeight)
                                .arg(card->height())));

    // ── 恢复路径：切走再切回，重建的条目同样要能点开、能长高 ──
    const QString other = controller.createSession(QDir::currentPath());
    panel.openSession(other);
    panel.openSession(mine);
    // 旧条目走 deleteLater：必须等它真正销毁，否则 findChild 会撞上上一轮的
    // （还挂在树上的）死部件——那份是展开状态，断言会被它骗过。
    QWidget* restored = nullptr;
    QTRY_COMPARE(panel.findChildren<QWidget*>(QStringLiteral("agentThinkingCard")).size(), 1);
    restored = panel.findChild<QWidget*>(QStringLiteral("agentThinkingCard"));
    QVERIFY(restored != nullptr);
    QLabel* restoredBody = bodyOf(restored);
    QVERIFY2(restoredBody != nullptr, "恢复后思考正文必须在");
    QVERIFY(!restoredBody->isVisible());
    const int restoredCollapsed = restored->height();
    QTest::mouseClick(restored, Qt::LeftButton, Qt::NoModifier, restored->rect().center());
    QTRY_VERIFY2(restoredBody->isVisible(), "恢复后点击必须能展开");
    QTRY_VERIFY2(restored->height() > restoredCollapsed + 20,
                 qPrintable(QStringLiteral("恢复后展开高度必须增长：%1 → %2")
                                .arg(restoredCollapsed)
                                .arg(restored->height())));
}

void AgentPanelSessionTest::pastedImageUsesTheSharedComposerAndReachesTheModel() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString imagePath = directory.filePath(QStringLiteral("pasted.png"));
    QImage image(32, 24, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::blue);
    QVERIFY(image.save(imagePath, "PNG"));

    auto model = std::make_unique<CapturingModel>();
    CapturingModel* captured = model.get();
    AgentController controller(std::move(model), QString());
    AgentChatPanel panel(controller);
    panel.resize(700, 500);
    panel.show();
    QVERIFY(QTest::qWaitForWindowExposed(&panel));
    const QString sessionId = controller.createSession(directory.path());
    panel.openSession(sessionId);

    QTextEdit* editor = panel.findChild<QTextEdit*>();
    QVERIFY(editor != nullptr);
    auto* mime = new QMimeData;
    mime->setUrls({QUrl::fromLocalFile(imagePath)});
    QApplication::clipboard()->setMimeData(mime);
    editor->setFocus();
    QTest::keyClick(editor, Qt::Key_V, Qt::ControlModifier);
    QTRY_VERIFY(editor->toPlainText().contains(QChar(0xFFFC)));
    const QString otherSession = controller.createSession(directory.path());
    panel.openSession(otherSession);
    QVERIFY(!editor->toPlainText().contains(QChar(0xFFFC)));
    panel.openSession(sessionId);
    QVERIFY(editor->toPlainText().contains(QChar(0xFFFC)));

    QPushButton* send =
        panel.findChild<QPushButton*>(QStringLiteral("agentSendButton"));
    QVERIFY(send != nullptr);
    QSignalSpy finished(&controller, &AgentController::turnFinished);
    QTest::mouseClick(send, Qt::LeftButton);
    QVERIFY(finished.wait(15000));

    const MaiModelRequest request = captured->request();
    QVERIFY(!request.messages.empty());
    QCOMPARE(request.messages.back().images.size(), std::size_t(1));
    QCOMPARE(QString::fromStdString(request.messages.back().images.front().path), imagePath);
    QVERIFY(panel.findChild<MarkdownView*>()->itemCount() >= 3);
    panel.openSession(sessionId);
    QVERIFY(panel.findChild<MarkdownView*>()->itemCount() >= 3);
}

void AgentPanelSessionTest::unconfiguredModelOpensConfigurationInsteadOfFailingTurn() {
    Harness harness;
    QVERIFY(QTest::qWaitForWindowExposed(harness.panel.get()));
    harness.panel->setModelLabel(QStringLiteral("未配置模型"));
    QSignalSpy requested(harness.panel.get(), &AgentChatPanel::modelConfigurationRequested);
    auto* editor = harness.panel->findChild<QTextEdit*>();
    QVERIFY(editor != nullptr);
    editor->setPlainText(QStringLiteral("你好"));
    QPushButton* send = harness.sendButton();
    QVERIFY(send != nullptr);
    QTest::mouseClick(send, Qt::LeftButton);

    QCOMPARE(requested.count(), 1);
    QCOMPARE(editor->toPlainText(), QStringLiteral("你好"));
}

void AgentPanelSessionTest::desktopAgentSuppliesMarkdownSystemPrompt() {
    auto model = std::make_unique<CapturingModel>();
    CapturingModel* observer = model.get();
    AgentController controller(std::move(model), QString());
    const QString sessionId = controller.createSession(QDir::currentPath());
    QVERIFY(controller.sendPrompt(sessionId, QStringLiteral("show repositories")));
    controller.agent().waitIdle();

    const MaiModelRequest request = observer->request();
    QVERIFY(!request.messages.empty());
    QCOMPARE(request.messages.front().role, MaiModelRole::User);
    const QString prompt = QString::fromStdString(request.baseInstructions);
    QVERIFY(prompt.contains(QStringLiteral("GitHub Flavored Markdown")));
    QVERIFY(prompt.contains(QStringLiteral("table row")));
    QVERIFY(prompt.contains(QStringLiteral("delimiter row")));
}

void AgentPanelSessionTest::composerMatchesImLayoutAndUsesEmbeddedSendAction() {
    Harness harness;
    harness.panel->resize(1200, 700);
    QVERIFY(QTest::qWaitForWindowExposed(harness.panel.get()));

    auto* editor = harness.panel->findChild<ComposerTextEdit*>();
    auto* card = harness.panel->findChild<QWidget*>(QStringLiteral("agentComposerCard"));
    auto* directory =
        harness.panel->findChild<QLabel*>(QStringLiteral("agentWorkingDirectory"));
    auto* policy =
        harness.panel->findChild<QPushButton*>(QStringLiteral("agentApprovalPolicy"));
    auto* model = harness.panel->findChild<QPushButton*>(QStringLiteral("agentModelChip"));
    auto* more = harness.panel->findChild<QPushButton*>(QStringLiteral("agentMoreActions"));
    auto* send = harness.panel->findChild<QPushButton*>(QStringLiteral("agentSendButton"));
    QVERIFY(editor != nullptr);
    QVERIFY(policy != nullptr);
    QVERIFY(model != nullptr);
    QVERIFY(more != nullptr);
    QVERIFY(send != nullptr);
    QCOMPARE(card, nullptr);
    QCOMPARE(directory, nullptr);
    QCOMPARE(send->parentWidget(), editor);
    QCOMPARE(send->text(), QString());
    QVERIFY(!send->icon().isNull());
    QCOMPARE(send->size(), QSize(UiZoom::s(36), UiZoom::s(36)));
    QVERIFY(editor->rect().contains(send->geometry().topLeft()));
    QVERIFY(editor->rect().contains(send->geometry().bottomRight()));
    QVERIFY(harness.panel->findChild<QPushButton*>(QStringLiteral("agentAttachImage")) == nullptr);
    QVERIFY(editor->width() >= harness.panel->width() - UiZoom::s(60));
    QVERIFY(editor->styleSheet().contains(QStringLiteral("border:1px solid")));
    QVERIFY(!editor->isAncestorOf(model));
    QVERIFY(!editor->isAncestorOf(policy));
    QVERIFY(more->menu() != nullptr);
    QVERIFY(more->menu()->actions().size() >= 2);
    QCOMPARE(more->menu()->actions()[0]->text(), QStringLiteral("模型配置"));
    QCOMPARE(more->menu()->actions()[1]->text(), QStringLiteral("清空当前对话"));
    QVERIFY(policy->menu() != nullptr);
    QVERIFY(policy->menu()->styleSheet().contains(QStringLiteral("QMenu::item:selected")));
    QCOMPARE(policy->menu()->actions().size(), 3);
    QCOMPARE(policy->menu()->actions()[0]->text(), QStringLiteral("请求批准"));
    QCOMPARE(policy->menu()->actions()[1]->text(), QStringLiteral("帮我批准"));
    QCOMPARE(policy->menu()->actions()[2]->text(), QStringLiteral("完全访问"));

}

void AgentPanelSessionTest::conversationAndComposerUseResizableSplitter() {
    Harness harness;
    harness.panel->resize(1200, 800);
    QVERIFY(QTest::qWaitForWindowExposed(harness.panel.get()));

    auto* splitter =
        harness.panel->findChild<QSplitter*>(QStringLiteral("agentMessageComposerSplitter"));
    QVERIFY(splitter != nullptr);
    QCOMPARE(splitter->orientation(), Qt::Vertical);
    QCOMPARE(splitter->count(), 2);
    QVERIFY(!splitter->childrenCollapsible());
    QCOMPARE(splitter->handleWidth(), 1);

    auto* editor = harness.panel->findChild<ComposerTextEdit*>();
    QVERIFY(editor != nullptr);
    QCOMPARE(editor->minimumHeight(), UiZoom::s(96));
    QVERIFY(editor->maximumHeight() > UiZoom::s(112));

    QSplitterHandle* handle = splitter->handle(1);
    QVERIFY(handle != nullptr);
    const int before = splitter->sizes().at(0);
    const int editorBefore = editor->height();
    const QPoint start = handle->rect().center();
    const QPoint finish = start + QPoint(0, -60);
    QTest::mousePress(handle, Qt::LeftButton, Qt::NoModifier, start);
    QMouseEvent move(QEvent::MouseMove, QPointF(finish), QPointF(handle->mapToGlobal(finish)),
                     Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(handle, &move);
    QTest::mouseRelease(handle, Qt::LeftButton, Qt::NoModifier, finish);
    QVERIFY(splitter->sizes().at(0) != before);
    QVERIFY(editor->height() > editorBefore);
}

void AgentPanelSessionTest::composerPlaceholderFitsAtMinimumHeight() {
    Harness harness;
    harness.panel->resize(1200, 700);
    QVERIFY(QTest::qWaitForWindowExposed(harness.panel.get()));
    auto* splitter =
        harness.panel->findChild<QSplitter*>(QStringLiteral("agentMessageComposerSplitter"));
    auto* editor = harness.panel->findChild<ComposerTextEdit*>();
    QVERIFY(splitter != nullptr);
    QVERIFY(editor != nullptr);
    splitter->setSizes({10000, 0});
    QApplication::processEvents();

    const qreal requiredHeight = editor->fontMetrics().lineSpacing()
        + 2 * editor->document()->documentMargin();
    QVERIFY2(editor->viewport()->height() >= requiredHeight,
             qPrintable(QStringLiteral("placeholder viewport %1px, one line needs %2px")
                            .arg(editor->viewport()->height()).arg(requiredHeight)));
    QVERIFY2(!editor->verticalScrollBar()->isVisible(),
             "An empty composer must not expose a scroll bar or its corner");
    QVERIFY2(!editor->horizontalScrollBar()->isVisible(),
             "An empty composer must not expose a horizontal scroll bar corner");
}

void AgentPanelSessionTest::conversationUsesAvailableWidth() {
    Harness harness;
    harness.panel->resize(1200, 700);
    QVERIFY(QTest::qWaitForWindowExposed(harness.panel.get()));

    MarkdownView* view = harness.view();
    QVERIFY(view != nullptr);
    view->addItem(QStringLiteral("wide-document"), MarkdownView::Style::Document,
                  QStringLiteral("内容应该使用窗口可用宽度"));
    QTRY_COMPARE(qRound(view->itemRect(QStringLiteral("wide-document")).left()), UiZoom::s(48));
    QCOMPARE(qRound(view->viewport()->width() -
                    view->itemRect(QStringLiteral("wide-document")).right()),
             UiZoom::s(48));
}

QTEST_MAIN(AgentPanelSessionTest)
#include "AgentPanelSessionTest.moc"
