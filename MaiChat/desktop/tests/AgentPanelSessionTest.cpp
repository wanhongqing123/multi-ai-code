#include <QApplication>
#include <QLabel>
#include <QPushButton>
#include <QSignalSpy>
#include <QDir>
#include <QtTest>

#include <atomic>
#include <memory>
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

}  // namespace

class AgentPanelSessionTest : public QObject {
    Q_OBJECT

private slots:
    void answerFromAnotherSessionDoesNotLeakIn();
    void anotherSessionFailingDoesNotShowAnError();
    void anotherSessionTitleDoesNotRenameTheHeader();
    void approvalFromAnotherSessionIsStillShown();
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

QTEST_MAIN(AgentPanelSessionTest)
#include "AgentPanelSessionTest.moc"
