#pragma once

#include <QString>
#include <QWidget>
#include <memory>

class AgentController;

// AI 助手的会话列表。
//
// ── 为什么要有它 ────────────────────────────────────────────────
//
// 一开始只做了一个固定会话，想法是"别让用户分不清在跟哪个说话"。真跑起来之后
// 这个想法站不住：**聊天记录是要回去翻的**。一个会话意味着上一件事的记录只能
// 靠"清空重来"扔掉，而扔掉之后就再也找不回来了。
//
// 核心本来就是多会话的（每个带自己的工作目录），这里只是把它显示出来。
//
// ── 和 IM 那边的联系人列表没有关系 ──────────────────────────────
//
// 它们长得像，但不共用一行代码：那边一项是"一个人"，带头像、未读数、在线状态；
// 这边一项是"一段工作"，带标题和工作目录。硬凑到一起会让两边都别扭。
class AgentSessionList final : public QWidget {
    Q_OBJECT

public:
    explicit AgentSessionList(AgentController& controller, QWidget* parent = nullptr);
    ~AgentSessionList() override;

    // 从存储重新拉一遍并重画。轮次结束、标题变了、新建会话之后都要调。
    void refresh();

    // 高亮某一项（不发 selected 信号，避免和调用方来回打架）。
    void setCurrent(const QString& sessionId);

signals:
    // 用户挑了一个会话。
    void selected(const QString& sessionId);
    // 用户按了"新对话"。**这里不建会话**——建在哪个目录是面板的事，
    // 列表只负责说"用户想开一个新的"。
    void newSessionRequested();

private:
    struct Runtime;
    std::unique_ptr<Runtime> runtime_;
};
