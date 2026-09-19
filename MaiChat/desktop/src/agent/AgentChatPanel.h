#pragma once

#include <QString>
#include <QWidget>
#include <memory>

class AgentController;
class QLabel;

// AI 助手的聊天页。
//
// ── 为什么不能照搬「跟人聊天」那一页 ──────────────────────────────
//
// 交互对齐 Codex 桌面端。差别不是审美偏好，是内容性质决定的：
//
//   **助手的回答不是气泡。** 人发的消息短，气泡合适；模型的回答动辄几百字带列表和代码，
//   塞进一个 520px 的窄气泡里就是一根面条，读起来极累。所以回答是整列宽的正文，
//   走 Markdown 渲染（复用 MaiChat 自己的 MarkdownRenderer），只有**用户**那一侧保留气泡。
//
//   **正文有一个居中的阅读列。** 窗口拉到 2000px 宽时正文不该跟着拉那么宽——
//   一行太长，眼睛回扫会丢行。
//
//   **输入区是一张卡，控件在卡里。** 工作目录、模型、发送都在同一个圆角容器的底边，
//   而不是散成三条横栏。目录是 agent 能碰到什么的边界，必须一直看得见，
//   但它不该占掉一整行。
//
// ── 另外两种非气泡的东西 ────────────────────────────────────────
//
//   思考条   一行淡色文字（「思考了 3 秒 ›」），点开看草稿。默认收着但**要动**——
//            真实的推理模型思考期能有十几秒，那段时间一个正文字都不会来，
//            屏幕上完全没反应的话和卡死分不开。
//   工具卡   一张窄卡。它表示「发生了一件事」，不是「谁说了句话」；做成气泡的话
//            一轮对话读起来像三个人在说话。等授权时就地长出按钮，不弹对话框——
//            弹窗会打断阅读，而且会训练用户条件反射点「允许」。
//
// 线程：只在主线程上跑。AgentController 已经把核心的事件排队搬过来了。
class AgentChatPanel final : public QWidget {
    Q_OBJECT

public:
    // controller 必须活得比这个面板久。面板只借着用，不接管。
    explicit AgentChatPanel(AgentController& controller, QWidget* parent = nullptr);
    ~AgentChatPanel() override;

    // 开一个会话。传空表示在当前目录建一个新的。
    void openSession(const QString& sessionId = QString());

    // 上下文条上显示哪个模型。
    //
    // **刻意让调用方给**，不在这儿写死一个名字当兜底：会话上的 model 字段为空是常态
    //（那表示「用 MaiAgent::Options 里的默认值」），而面板看不见那个默认值。
    // 自己编一个名字显示出去，用户看到的就可能不是实际在跑的那个。
    void setModelLabel(const QString& model);

    QString sessionId() const;

signals:
    // 会话列表该重拉了。三种时机：新建了一个、标题被自动填上了、
    // 一轮跑完（updated 变了，列表按它排序，位置会动）。
    void sessionListChanged();

private:
    // 这三个是这个面板专用的部件，别处用不上，所以做成嵌套私有类、定义在 .cpp 里。
    class AnswerView;
    class ThinkingLine;
    class ToolCard;

    // 把整个对话从存储里重画一遍。用在打开会话和清空之后——
    // 流式期间**不要**调它，那会把正在长的正文整个换掉，界面会闪。
    void reloadFromStore();

    void onSend();
    void onClear();

    // 流式期间按 partId 找（或建）对应的部件，只追加不重画。
    AnswerView* answerViewFor(const QString& partId);
    ThinkingLine* thinkingLineFor(const QString& partId);
    ToolCard* toolCardFor(const QString& partId);
    void refreshToolCard(const QString& messageId, const QString& partId);
    void showApproval(const QString& permissionId);

    // 按钮按下时才去查 permissionId。存一份在卡片上的话，
    // 多端同时开着时别人先裁决了，这里存的就是个过期的 id。
    void replyForPart(const QString& partId, bool forSession, bool approve);

    void appendUserBubble(const QString& text);
    void appendNotice(const QString& text, bool isError);
    void addToStream(QWidget* widget, Qt::Alignment alignment);
    void setRunning(bool running);
    void scrollToBottom();
    void refreshContextSize();

    struct Runtime;
    std::unique_ptr<Runtime> runtime_;
};
