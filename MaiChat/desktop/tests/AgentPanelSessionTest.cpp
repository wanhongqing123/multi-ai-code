#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QImage>
#include <QLabel>
#include <QMenu>
#include <QMimeData>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTextEdit>
#include <QUrl>
#include <QtTest>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

#include "agent/AgentChatPanel.h"
#include "agent/AgentController.h"
#include "markdown/MarkdownView.h"

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

}  // namespace

class AgentPanelSessionTest : public QObject {
    Q_OBJECT

private slots:
    void answerFromAnotherSessionDoesNotLeakIn();
    void anotherSessionFailingDoesNotShowAnError();
    void anotherSessionTitleDoesNotRenameTheHeader();
    void approvalFromAnotherSessionIsStillShown();
    void thinkingLineExpandsLiveAndAfterRestore();
    void unconfiguredModelOpensConfigurationInsteadOfFailingTurn();
    void desktopAgentSuppliesMarkdownSystemPrompt();
    void composerUsesApplicationStyleWithoutInheritedLabelBorders();
    void pastedImageUsesTheSharedComposerAndReachesTheModel();
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
        for (QPushButton* button : panel->findChildren<QPushButton*>()) {
            if (button->text() == QStringLiteral("发送") ||
                button->text() == QStringLiteral("停止")) {
                return button;
            }
        }
        return nullptr;
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
    auto* send = [this, &panel]() -> QPushButton* {
        for (QPushButton* button : panel.findChildren<QPushButton*>()) {
            if (button->text() == QStringLiteral("发送")) return button;
        }
        return nullptr;
    }();
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

    QPushButton* send = nullptr;
    for (QPushButton* button : panel.findChildren<QPushButton*>())
        if (button->text() == QStringLiteral("发送")) send = button;
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

void AgentPanelSessionTest::composerUsesApplicationStyleWithoutInheritedLabelBorders() {
    Harness harness;
    QVERIFY(QTest::qWaitForWindowExposed(harness.panel.get()));

    auto* card = harness.panel->findChild<QWidget*>(QStringLiteral("agentComposerCard"));
    auto* directory =
        harness.panel->findChild<QLabel*>(QStringLiteral("agentWorkingDirectory"));
    auto* policy =
        harness.panel->findChild<QPushButton*>(QStringLiteral("agentApprovalPolicy"));
    auto* send = harness.panel->findChild<QPushButton*>(QStringLiteral("agentSendButton"));
    QVERIFY(card != nullptr);
    QVERIFY(directory != nullptr);
    QVERIFY(policy != nullptr);
    QVERIFY(send != nullptr);
    QVERIFY(card->styleSheet().contains(QStringLiteral("QFrame#agentComposerCard")));
    QVERIFY(!card->styleSheet().contains(QStringLiteral("QFrame{")));
    QVERIFY(policy->menu() != nullptr);
    QVERIFY(policy->menu()->styleSheet().contains(QStringLiteral("QMenu::item:selected")));
    QCOMPARE(policy->menu()->actions().size(), 3);
    QCOMPARE(policy->menu()->actions()[0]->text(), QStringLiteral("请求批准"));
    QCOMPARE(policy->menu()->actions()[1]->text(), QStringLiteral("帮我批准"));
    QCOMPARE(policy->menu()->actions()[2]->text(), QStringLiteral("完全访问"));

    const QString rootSession = harness.controller->createSession(QDir::rootPath());
    harness.panel->openSession(rootSession);
    QCOMPARE(directory->text(), QDir::toNativeSeparators(QDir::rootPath()));
}

QTEST_MAIN(AgentPanelSessionTest)
#include "AgentPanelSessionTest.moc"
