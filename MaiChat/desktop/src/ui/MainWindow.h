#pragma once

#include <QHash>
#include <QLabel>
#include <QListWidget>
#include <QMainWindow>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QTextEdit>
#include <QVBoxLayout>

#include "app/RemoteIMApplication.h"

class QResizeEvent;
class QShowEvent;
class QLineEdit;
class QHBoxLayout;
class QListWidgetItem;
class QPoint;
class QTimer;
class QImage;

class MainWindow final : public QMainWindow {
public:
    explicit MainWindow(RemoteIMApplication& app, QWidget* parent = nullptr);

    // 把 IM 收/发的文件附件从本地缓存拷贝到 targetPath（存在则覆盖）。
    // 纯文件操作、不弹 UI，供右键「保存到本地」与单测复用；失败时经
    // errorMessage 返回原因。
    static bool copyAttachmentToPath(const RemoteIMFileAttachment& attachment,
                                     const QString& targetPath,
                                     QString* errorMessage = nullptr);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    void buildUi();
    void applyStyle();
    void bindSignals();
    void refresh();
    void refreshContacts();
    void refreshContactDirectory();
    void refreshSettings();
    void refreshMessages();
    void rebuildMessageList(const QString& peerId, const QList<RemoteIMMessage>& messages);
    void applyIncrementalMessageUpdate(const QList<RemoteIMMessage>& messages);
    void updateLoadEarlierVisibility();
    void scrollMessagesToBottom();
    void applyConversationFilter();
    void showMessagesPage();
    void showContactsPage();
    void showSettingsPage();
    void syncNavigationSelection();
    void updateNavigationSelection(QPushButton* selectedButton);
    void openAddContactDialog();
    // 处理消息框里的 Ctrl+V：把剪贴板里的图片/文件作为内联对象插入到输入框中（不立即发送），
    // 返回 true 表示已消费；否则返回 false 交给 QTextEdit 默认粘贴文本。
    bool handleComposerPaste();
    void insertComposerImage(const QImage& image);
    void insertComposerFile(const QString& localPath);
    // 输入框里一枚内联附件：isFile 区分文件卡（true）与图片（false），path 为本地原文件路径。
    struct ComposerAttachment {
        bool isFile;
        QString path;
    };
    // 按文档顺序取出输入框里内联的图片/文件附件（发送时用于与配文合并）。
    QList<ComposerAttachment> collectComposerAttachments() const;
    // 输入框里是否有内联的图片/文件附件。
    bool composerHasAttachments() const;
    void openImagePreview(const QString& imagePath);
    void openFilePreview(const RemoteIMFileAttachment& attachment);
    // 右键菜单入口：弹「另存为」对话框（默认下载目录 + 原文件名），把附件保存到用户选的位置。
    void saveFileAttachmentToLocal(const RemoteIMFileAttachment& attachment);
    QWidget* createMessageBubble(const RemoteIMMessage& message);
    int messageBubbleMaximumWidth() const;
    void applyMessageBubbleWidth(QWidget* bubble, bool expanded) const;
    void updateMessageBubbleWidths();
    QWidget* createSettingsRow(const QString& title, QLabel* valueLabel, const QString& helperText);
    void sendCurrentText();
    void updateComposerState();
    void updateSlashCommandSuggestions();
    void positionSlashCommandBar();
    void selectSlashCommand(const QString& command);
    // 会话列表右键：只提供「删除消息」（清空聊天记录、好友保留）。
    // 「删除好友」是通讯录（contactsList_）的专属功能。
    void showConversationContextMenu(const QPoint& pos);
    void showContactContextMenu(QListWidget* list, const QPoint& pos);
    void deleteContactFromItem(QListWidgetItem* item);
    void clearMessagesFromItem(QListWidgetItem* item);
    void deleteSelectedContactFromList(QListWidget* list);
    QString contactName(const QString& userId) const;

    RemoteIMApplication& app_;
    QWidget* navRail_ = nullptr;
    QLineEdit* navSearchInput_ = nullptr;
    QPushButton* messageNavButton_ = nullptr;
    QPushButton* contactsNavButton_ = nullptr;
    QPushButton* settingsNavButton_ = nullptr;
    QStackedWidget* contentStack_ = nullptr;
    QWidget* messagesPage_ = nullptr;
    QWidget* contactsPage_ = nullptr;
    QWidget* settingsPage_ = nullptr;
    QListWidget* conversationList_ = nullptr;
    QListWidget* contactsList_ = nullptr;
    QLabel* titleLabel_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QLabel* settingsAccountValue_ = nullptr;
    QLabel* settingsConnectionValue_ = nullptr;
    QLabel* settingsSdkAppIdValue_ = nullptr;
    QScrollArea* messageScroll_ = nullptr;
    QWidget* messageContainer_ = nullptr;
    QVBoxLayout* messageLayout_ = nullptr;
    // One-shot connection used to jump to the latest message once the message
    // list layout has settled (bubble heights depend on width / word wrap, so
    // the scrollbar range is only correct after a later layout pass).
    QMetaObject::Connection messageScrollToBottomConn_;
    // 增量渲染状态：只有切换会话/空态变化才全量重建；平时新消息 append、
    // 翻页 prepend、状态变化原位替换，避免大历史下整屏重建气泡。
    QString renderedPeerId_;
    QStringList renderedMessageIds_;
    QHash<QString, QWidget*> messageRowById_;
    QHash<QString, RemoteIMMessageStatus> renderedStatusById_;
    QPushButton* loadEarlierButton_ = nullptr;
    bool renderedEmptyView_ = false;
    QPushButton* addContactButton_ = nullptr;
    QPushButton* voiceButton_ = nullptr;
    QPushButton* sendButton_ = nullptr;
    QTextEdit* messageEditor_ = nullptr;
    QWidget* slashCommandBar_ = nullptr;
    QVBoxLayout* slashCommandLayout_ = nullptr;
    // 命令提示条重建从 textChanged（键盘事件派发内）里剥离出来，改由防抖单次定时器
    // 延后执行——只在停顿时重建，绝不在按键前后那一瞬间动控件。否则在按键派发/输入法
    // 组词期间同步删除全部按钮并隐藏/抬升悬浮层，会在 Windows 上吞掉 KeyRelease（按键
    // 卡住自动重复，/g 变成一长串 g），或打断输入法上下文（首个拼音键被当普通字符漏进
    // 输入框，/goal 后打 nihao 变成字面 n + 组词 ihao）。
    QTimer* slashCommandUpdateTimer_ = nullptr;
    // 输入法是否正在组词（预编辑串非空）。组词期间一律不重建命令栏，组词结束再刷新。
    bool imeComposing_ = false;
    // 「保存到本地」上次使用的目录：同一会话内多次保存不必每次从下载目录重新翻。
    QString lastAttachmentSaveDir_;
};
