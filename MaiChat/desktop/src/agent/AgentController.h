#pragma once

#include <QObject>
#include <QString>
#include <QVector>
#include <memory>

#include "MaiAgent.h"

// MaiAgent 的事件是普通 C++ 回调，不是 Qt 信号，而且**不在主线程上触发**。
// 把它排队搬到 Qt 的事件循环里，是这个类存在的全部理由。
//
// ---- 为什么不能让界面直接订阅事件总线 ----
//
// MaiEventBus 的处理函数在 publish 的那个线程上同步调用，
// 流式期间那是网络读线程（见 MaiEventBus.h 的线程契约）。在那里碰 QWidget 是未定义行为；
// 而且那条契约还规定处理函数里不许做慢活——查一次数据库、刷一次界面都算，会直接拖慢模型吐字。
//
// 所以这里的处理函数只做一件事：把事件原样 emit 出去。
// 跨线程的信号是**排队**投递的（接收方在主线程，Qt 自动选 QueuedConnection），
// 投递本身只是加锁往队列塞一个指针，符合那条契约。真正的排版、查库、刷界面都发生在主线程的槽里。
//
// ---- 正文和思考过程为什么要分两个信号 ----
//
// 两种增量在协议上**长得一模一样**（field 都是 "text"），
// 唯一的区别是 partId 指向的片段是 MaiTextPart 还是 MaiReasoningPart。
// 要分清就得拿 partId 去查存储——那是慢活，
// 只能在主线程做。所以这个类在主线程维护一张 partId -> 类型的小表：
// 每个片段第一次出现时查一次记下来，之后的增量直接查表。
//
// 这件事必须做对：思考过程是模型的草稿，当成正文显示出来的话，
// 用户看到的是一段语无伦次的自言自语接着真答案。MaiAgent 那边踩过一次，有回归用例盯着。
//
// ---- 用法 ----
//
//   auto* agent = new AgentController(config, dbPath, this);
//   connect(agent, &AgentController::textDelta, this, &ChatView::appendText);
//   connect(agent, &AgentController::permissionAsked, this, &ChatView::showApprovalCard);
//   const QString sid = agent->createSession(QDir::currentPath());
//   agent->sendPrompt(sid, "你好");      // 立刻返回，回答从信号里来
class AgentController final : public QObject {
    Q_OBJECT

public:
    struct ModelConfig {
        // 不带末尾斜杠，也不带具体路径。例如：
        //   https://open.bigmodel.cn/api/coding/paas/v4   GLM 编程套餐
        //   http://127.0.0.1:11434/v1                     Ollama
        QString baseUrl;
        QString apiKey;
        QString modelName = QStringLiteral("glm-5.3");
    };

    // databasePath 为空表示纯内存：进程退出后会话和消息都不留。
    //
    // model.baseUrl 为空表示空转——建会话、翻历史照常，发消息会以"没有配置模型" 收场。
    // 界面还没配好模型时是这个状态。
    AgentController(const ModelConfig& model, const QString& databasePath,
                    QObject* parent = nullptr);

    // 自己塞一个模型实现进来。给测试用，也给以后接别的供应商留的口子。
    AgentController(std::unique_ptr<MaiModelClient> model, const QString& databasePath,
                    QObject* parent = nullptr);

    ~AgentController() override;

    // 打开数据库失败时非空，此时这个对象仍然可用（退化成纯内存）。
    QString openError() const;

    // 底层核心。
    // 查询类操作（listMessages / listSessions / listPendingPermissions）直接用它——**在主线程上查没
    // 有任何限制**，那条"不许做慢活"的契约只管事件处理函数。
    MaiAgent& agent();

    // ---- 变更 ----都是立刻返回的。失败时返回空串 / false，错误文本从 lastError() 取。

    QString createSession(const QString& directory, const QString& title = QString());
    bool sendPrompt(const QString& sessionId, const QString& text);
    bool interrupt(const QString& sessionId);

    // approveForSession = true 表示"这个会话里这个工具以后别再问"。
    bool approvePermission(const QString& permissionId, bool approveForSession = false);
    bool denyPermission(const QString& permissionId);

    QString lastError() const;

signals:
    // 模型说给用户听的话，一次一小段。**不带全量**，界面自己往后追加。
    void textDelta(const QString& sessionId, const QString& messageId, const QString& partId,
                   const QString& delta);

    // 模型的思考过程。界面通常折叠显示，或者干脆不显示——但**至少要让用户知道它在想**：
    // 真实的推理模型思考期能有十几秒，这段时间里一个正文字符都不会来，屏幕上没反应和卡死分不开。
    void reasoningDelta(const QString& sessionId, const QString& messageId,
                        const QString& partId, const QString& delta);

    // 某个工具片段的状态变了（建卡 / 放行 / 跑完）。事件只带 id，工具名、参数、
    // 输出要拿 partId 去 agent().listMessages() 里取——主线程查，随便查。
    void toolPartChanged(const QString& sessionId, const QString& messageId,
                         const QString& partId);

    // 有一次工具调用在等用户点头。详情同样去查：agent().listPendingPermissions()。
    void permissionAsked(const QString& permissionId, const QString& sessionId);
    // 裁决落地了。**包括不是本界面发起的那次**——多端同时开着时这是唯一的同步手段。
    void permissionReplied(const QString& permissionId, const QString& decision);

    // 这一轮结束了。界面据此收起"正在输入"，并把输入框解锁。
    void turnFinished(const QString& sessionId);
    // 这一轮出错了。**用户主动中断不会走这里**——那不是故障。
    void turnFailed(const QString& sessionId, const QString& message);
    // 会话标题变了（第一轮结束后会用用户那句话自动填上）。
    void sessionTitleChanged(const QString& sessionId, const QString& title);

private:
    // 事件从核心的线程过来，经这个信号排队到主线程。私有：界面不该直接连它，
    // 它带的是未经分类的原始事件。
    void rawEvent(const MaiEvent& event);
    Q_SIGNAL void eventQueued(const MaiEvent& event);
    Q_SLOT void onEventQueued(const MaiEvent& event);

    struct Runtime;
    std::unique_ptr<Runtime> runtime_;
};

// 跨线程排队投递必须知道怎么拷贝这个类型。少了这一行，
// 连接会在运行期报 "Cannot queue arguments of type MaiEvent"，事件静默丢掉——表现是界面完全不动，
// 而没有任何报错。
Q_DECLARE_METATYPE(MaiEvent)
