#pragma once

#include <QString>
#include <QStringList>
#include <QWidget>
#include <memory>

class AgentController;
class QImage;
class QLabel;
class QMimeData;
enum class MaiApprovalPolicy;

// AI 助手的聊天页。
//
// ── 为什么不能照搬「跟人聊天」那一页 ──────────────────────────────
//
// 交互对齐 Codex 桌面端。差别不是审美偏好，是内容性质决定的：
//
//   **助手的回答不是气泡。** 人发的消息短，气泡合适；模型的回答动辄几百字带列表和代码，
//   塞进一个 520px 的窄气泡里就是一根面条，读起来极累。所以回答是整列宽的正文，
//   只有**用户**那一侧保留气泡。
//
//   **整个展示区是一个 MarkdownView**，不是一条消息一个部件。它自己排版自己绘制，
//   按可见区裁剪；跨消息选区、复制原始 Markdown、链接命中都在那一层。
//   主题和 IM 共用一份 MarkdownTheme——外观要调只改一个地方。
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
//   工具行   默认只显示一行摘要，点开才看命令与输出。它表示「发生了一件事」，
//            不是「谁说了句话」。等授权时就地长出按钮，不弹对话框——
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
    void modelConfigurationRequested();
    void modelSelected(const QString& model);

private:
    // 这两个是这个面板专用的部件，别处用不上，所以做成嵌套私有类、定义在 .cpp 里。
    // 它们要能点（展开、授权），画不出来，所以嵌进 MarkdownView 里当部件用。
    class ThinkingLine;
    class ToolCard;
    class SubAgentCard;

    // 把整个对话从存储里重画一遍。用在打开会话和清空之后——
    // 流式期间**不要**调它，那会把正在长的正文整个换掉，界面会闪。
    void reloadFromStore();

    // 这条事件是不是当前这个会话的。
    //
    // 子 Agent 并发跑在别的会话里，**正文类的事件必须滤掉**，否则它的回答会
    // 接进父的答案里。授权和提问是例外：子 Agent 没有自己的界面，滤掉的话
    // 它会永远挂在闸门上等一个永远不会来的点头。
    bool isCurrentSession(const QString& sessionId) const;

    // 别的会话在动。是当前会话的子 Agent 就更新它那张卡，不是就丢掉。
    //
    // **子任务的正文不进主对话流**：那是它自己的活，父要的是结论，
    // 而结论会通过 wait_agent 回到父这边、正常出现在回答里。全铺出来的话
    // 一屏里三个 agent 同时说话，谁也读不下去。
    void noteOtherSession(const QString& sessionId, const QString& text);
    SubAgentCard* subAgentCardFor(const QString& sessionId);

    void onSend();
    void onClear();
    bool insertComposerMimeData(const QMimeData* mime);
    void insertComposerImage(const QImage& image);
    void insertComposerImageFile(const QString& path);
    QStringList composerImagePaths() const;
    void appendUserImages(const QStringList& paths);

    // 流式期间往某条回答后面追加。攒一小会儿再刷进视图，
    // 每个 delta 都重排是 O(n^2)。
    void appendAnswerDelta(const QString& partId, const QString& delta);
    void flushAnswers();

    // 流式期间按 partId 找（或建）对应的部件，只追加不重画。
    ThinkingLine* thinkingLineFor(const QString& partId);
    ToolCard* toolCardFor(const QString& partId);
    void refreshToolCard(const QString& messageId, const QString& partId);
    void showApproval(const QString& permissionId);

    // 模型中途问了一句。
    //
    // **不另起一个对话框，复用输入框。** 弹窗会打断阅读，而且这本来就是一次对话——
    // 问题当成一条消息显示出来，用户照常在下面打字回答，只是这一次回车发的是答案
    // 而不是新的 prompt。
    void showQuestion(const QString& questionId);
    void clearQuestion();

    // 按钮按下时才去查 permissionId。存一份在卡片上的话，
    // 多端同时开着时别人先裁决了，这里存的就是个过期的 id。
    void replyForPart(const QString& partId, bool forSession, bool approve);

    void appendUserBubble(const QString& text);
    void appendNotice(const QString& text, bool isError);
    void setRunning(bool running);
    void scrollToBottom();
    void refreshContextSize();
    void selectApprovalPolicy(MaiApprovalPolicy policy);
    void updateApprovalPolicyUi();

    struct Runtime;
    std::unique_ptr<Runtime> runtime_;
};
