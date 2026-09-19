#pragma once

#include <QString>
#include <QWidget>
#include <memory>

class AgentController;
class QLabel;

// AI 助手的聊天页。
//
// 它不是「跟人聊天那个页面换个头像」。三处不一样，每一处都有具体理由：
//
//   输入区   表情 / 图片 / 附件全去掉——agent 不吃这些，留着就是点了没反应的死按钮。
//            换成「@ 引用文件」（只插路径，不上传）和一条常驻的上下文条：
//            **工作目录和模型一直摆在那儿**。目录是它能碰到什么的边界，
//            越界会被核心的 maiResolvePathWithinRoot 挡掉，这种东西藏进菜单就没人看了。
//            输入框三行起：一行高的框会逼着人把任务写短，而任务写短的直接后果是它猜错。
//
//   对话流   除了气泡还有两种东西：思考条和工具卡。它们**不是气泡**——
//            气泡表示「谁说了句话」，这两个表示「发生了一件事」。
//            混成气泡的话，一轮对话读起来像三个人在说话。
//
//   顶上     一条「攒了多少字 · 清空重来」。协议是无状态的，历史每一轮都要全量重发，
//            聊得越久每句话越贵越慢。把这个数摆出来，用户才有理由去点清空。
//
// 线程：这个类只在主线程上跑。AgentController 已经把核心的事件排队搬过来了，
// 这里收到的信号全部在主线程上——查存储、刷部件都随意。
class AgentChatPanel final : public QWidget {
    Q_OBJECT

public:
    // controller 必须活得比这个面板久。面板只借着用，不接管。
    explicit AgentChatPanel(AgentController& controller, QWidget* parent = nullptr);
    ~AgentChatPanel() override;

    // 开一个会话。传空表示在当前目录建一个新的——界面上只有一个固定的
    // AI 助手会话，所以正常路径是启动时建一次，之后一直用它。
    void openSession(const QString& sessionId = QString());

    // 上下文条上显示哪个模型。
    //
    // **刻意让调用方给**，不在这儿写死一个名字当兜底：会话上的 model 字段为空是常态
    // （那表示"用 MaiAgent::Options 里的默认值"），而面板看不见那个默认值。
    // 自己编一个名字显示出去，用户看到的就可能不是实际在跑的那个。
    void setModelLabel(const QString& model);

    QString sessionId() const;

private:
    // 这两个是这个面板专用的部件，别处用不上，所以做成嵌套私有类、定义在 .cpp 里。
    // 放全局会白白占掉 ThinkingStrip / ToolCard 这两个挺通用的名字。
    class ThinkingStrip;
    class ToolCard;

    // 把整个对话从存储里重画一遍。用在打开会话和清空之后——
    // 流式期间**不要**调它，那会把正在长的气泡整个换掉，界面会闪。
    void reloadFromStore();

    void onSend();
    void onClear();

    // 流式期间按 partId 找（或建）对应的部件，只追加不重画。
    QLabel* textBubbleFor(const QString& partId);
    ThinkingStrip* thinkingStripFor(const QString& partId);
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
