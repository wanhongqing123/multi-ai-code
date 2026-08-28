#pragma once

#include <QHash>
#include <QSet>
#include <QLabel>
#include <QListWidget>
#include <QMainWindow>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QTextEdit>
#include <QVBoxLayout>
#include <functional>
#include <memory>

#include "app/RemoteIMApplication.h"
#include "remote/RemoteDesktopController.h"
#include "remote/RemoteDesktopSettings.h"
#include "ui/RemoteInputCapture.h"

class SharingIndicatorBar;
class RemoteDesktopViewPanel;
class QButtonGroup;
class QCheckBox;
class QCloseEvent;
class QMimeData;
class QResizeEvent;
class QShowEvent;
class QLineEdit;
class QHBoxLayout;
class QListWidgetItem;
class QPoint;
class QTimer;
class QImage;
class ImagePreviewDialog;

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
    void closeEvent(QCloseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    void buildUi();
    void applyStyle();
    void bindSignals();
    void refresh();
    void refreshSelectedConversation();
    void refreshContacts();
    void refreshContactDirectory();
    // 通讯录分组：表头和联系人共用 contactsList_，表头是一种特殊行。
    QListWidgetItem* makeContactItem(const RemoteIMContact& contact);
    void appendContactGroupSection(const QString& groupName,
                                   const QList<RemoteIMContact>& members);
    void toggleContactGroupCollapsed(const QString& groupName);
    // 返回真正建出来的分组名；用户取消或名字不合法时返回空串。
    QString createContactGroup();
    // preselectedGroup 非空时，打开对话框就预先勾中该分组（分组表头右键进来的情况）。
    void openBroadcastDialog(const QString& preselectedGroup = QString());
    void reportBroadcastResult(int total, const QStringList& failedPeerIds);
    void renameContactGroup(const QString& groupName);
    void deleteContactGroup(const QString& groupName);
    void appendMoveToGroupMenu(QMenu& menu, const QString& userId, const QString& currentGroup);
    void showContactGroupContextMenu(QListWidget* list, QListWidgetItem* item, const QPoint& pos);
    void refreshSettings();
    void refreshMessages();
    void rebuildMessageList(const QString& peerId, const QList<RemoteIMMessage>& messages);
    void applyIncrementalMessageUpdate(const QList<RemoteIMMessage>& messages);
    void updateLoadEarlierVisibility();
    void scrollMessagesToBottom();
    void applyConversationFilter();
    void applyContactFilter();
    // 搜索按页分工：消息页搜消息（顺带把有命中的会话留在左列），
    // 通讯录页搜联系人。Ctrl+F 聚焦当前页的那个框，Esc 清空/收起结果面板。
    void focusPageSearch();
    void focusGlobalSearch();
    void refreshGlobalSearchResults();
    void openGlobalSearchResult(QListWidgetItem* item);
    void closeGlobalSearchResults();
    void layoutGlobalSearchResults();
    // 搜索状态下点左列会话时，落到该会话里最贴切的那条命中。
    // 排序规则与结果面板一致（先分数、后时间），否则同一次搜索里两个入口会跳到不同消息。
    QString bestSearchHitId(const QString& peerId, const QString& needle) const;
    void highlightMessage(const QString& messageId);
    void clearMessageSearchHighlight();
    void showMessagesPage();
    void showContactsPage();
    void showSettingsPage();
    void showRemotePage();
    void syncNavigationSelection();
    void updateNavigationSelection(QPushButton* selectedButton);
    void openAddContactDialog();
    // 处理消息框里的 Ctrl+V：把剪贴板里的图片/文件作为内联对象插入到输入框中（不立即发送），
    // 返回 true 表示已消费；否则返回 false 交给 QTextEdit 默认粘贴文本。
    bool handleComposerPaste();
    // 粘贴与拖拽共用的 mime 路由：本地文件 URL → 内联附件，图像数据 → 内联图片。
    // 返回 true 表示已消费；false 交给 QTextEdit 默认处理（插入文本）。
    // 拖拽必须走这里：QTextEdit 默认会把 file:/// URL 当成纯文本插进输入框，
    // 于是「拖一个文件进来」变成发出一行路径字符串，附件根本没产生。
    bool insertComposerMimeData(const QMimeData* mime);
    void insertComposerImage(const QImage& image);
    // 本地图片文件：按原文件路径内联，发送时直接发原图，不做 PNG 重编码。
    void insertComposerImageFile(const QString& localPath);
    void insertComposerFile(const QString& localPath);
    void insertComposerVideo(const QString& localPath);
    // 文件/视频在输入框里都用一枚「卡片」缩略图表示。两者只差图标和资源名前缀，
    // 画法共用这一处。
    void insertComposerChip(const QString& localPath, const QString& icon, const QString& resourcePrefix);
    // 输入框里一枚内联附件：kind 决定发送时走哪条 IM 通道，path 为本地原文件路径。
    struct ComposerAttachment {
        enum class Kind { Image, File, Video };
        Kind kind;
        QString path;
    };
    // 按文档顺序取出输入框里内联的图片/文件附件（发送时用于与配文合并）。
    QList<ComposerAttachment> collectComposerAttachments() const;
    // 输入框里是否有内联的图片/文件附件。
    bool composerHasAttachments() const;
    void openImagePreview(const QString& imagePath);
    void openFilePreview(const RemoteIMFileAttachment& attachment);
    void openVideoPreview(const RemoteIMVideoAttachment& attachment);
    // 右键菜单入口：弹「另存为」对话框（默认下载目录 + 原文件名），把附件保存到用户选的位置。
    void saveFileAttachmentToLocal(const RemoteIMFileAttachment& attachment);
    QWidget* createMessageBubble(const RemoteIMMessage& message);
    enum class ApprovalDisplayState { Available, Sending, Sent, Resolved, AutoDeclined };
    ApprovalDisplayState approvalDisplayState(const RemoteIMMessage& message) const;
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
    // 整体缩放（飞书式 Ctrl+= / Ctrl+- / Ctrl+0）：改倍率 → 重放全局字体与样式表、
    // 重建列表与消息气泡，并弹出百分比提示浮层。
    void changeUiZoom(qreal delta);
    void resetUiZoom();
    void applyUiZoom(bool showToastPopup);
    // 代码级最小宽高（不在样式表里）需要在每次倍率变化时重放，否则放大后
    // 再缩小会被旧的大最小值卡住，布局缩不回去。
    void applyScaledFixedGeometry();
    void showZoomToast();
    // 远程桌面入口：可用性随会话选中状态与 TRTC 是否编译进来变化。
    void requestRemoteDesktop();
    void updateRemoteDesktopButton();
    void setupRemoteDesktop();
    void handleRemoteDesktopConsent(const QString& fromUserId);
    void openRemoteDesktopViewer(const QString& peerUserId);
    void closeRemoteDesktopViewer();
    void stopRemoteDesktopForShutdown(std::function<void()> completion = {});
    void applyRemoteDesktopFullScreen(bool fullScreen);
    void toggleRemoteDesktopControl(const QString& peerUserId);
    void promptRemoteDesktopPassword(const QString& peerUserId);
    QWidget* buildRemoteDesktopSettingsPanel(QWidget* parent);
    void refreshRemoteDesktopSettings();
    void editRemoteDesktopPassword();
    void editRemoteDesktopAllowList();
    void editRemoteDesktopProxy();
    void showConversationContextMenu(const QPoint& pos);
    void showContactContextMenu(QListWidget* list, const QPoint& pos);
    void deleteContactFromItem(QListWidgetItem* item);
    void clearMessagesFromItem(QListWidgetItem* item);
    void deleteSelectedContactFromList(QListWidget* list);
    QString contactName(const QString& userId) const;

    RemoteIMApplication& app_;
    QWidget* navRail_ = nullptr;
    QLineEdit* navSearchInput_ = nullptr;
    QLineEdit* contactsSearchInput_ = nullptr;
    QPushButton* newContactGroupButton_ = nullptr;
    QPushButton* broadcastButton_ = nullptr;
    // 折叠状态只活在内存里：重启后一律展开。持久化它的收益很小，
    // 而"上次收起来的组这次还是收着的"反而容易让人以为联系人少了。
    QSet<QString> collapsedContactGroups_;
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
    QPushButton* remoteDesktopButton_ = nullptr;
    SharingIndicatorBar* sharingIndicator_ = nullptr;
    RemoteDesktopViewPanel* remoteDesktopView_ = nullptr;
    // 进全屏前窗口是不是最大化的：退出时要还原成原样，不能一律 showNormal。
    bool remoteFullScreenWasMaximized_ = false;
    std::unique_ptr<RemoteInputCapture> remoteInputCapture_;
    // TRTC 接收端编码帧尺寸。不能用 1920x1080 猜：source-aspect 策略生效后
    // 可能是 1728x1080；首帧回调前保持空，坐标采集也保持禁用。
    QSize remoteDesktopRemoteVideoSize_;
    QWidget* remotePage_ = nullptr;
    QPushButton* remoteNavButton_ = nullptr;
    RemoteDesktopController* remoteDesktop_ = nullptr;
    bool remoteDesktopShutdown_ = false;
    bool remoteDesktopShutdownComplete_ = false;
    std::unique_ptr<RemoteDesktopSettingsStore> remoteDesktopSettingsStore_;
    // 对端设了访问密码时，本次运行内记住，避免每次重试都问。不落盘。
    QHash<QString, QString> remoteDesktopPasswords_;
    QButtonGroup* remoteDesktopModeGroup_ = nullptr;
    QLabel* remoteDesktopPasswordValue_ = nullptr;
    QLabel* remoteDesktopAllowValue_ = nullptr;
    QLabel* remoteDesktopControlValue_ = nullptr;
    QCheckBox* remoteDesktopControlToggle_ = nullptr;
    QLabel* remoteDesktopProxyValue_ = nullptr;
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
    // 与消息状态一并决定是否原位重建审批气泡。
    QHash<QString, ApprovalDisplayState> renderedApprovalStateById_;
    // 只保存一次网络请求的瞬时状态；成功状态由已落库的出站审批决定恢复。
    QSet<QString> submittingApprovalTokens_;
    QSet<QString> sentApprovalTokens_;
    QSet<QString> resolvedApprovalTokens_;
    QSet<QString> autoDeclinedApprovalTokens_;
    QPushButton* loadEarlierButton_ = nullptr;
    // 顶栏搜索。范围是所有会话里「已加载」的消息——没点过「加载更早」的历史
    // 不在 ChatState 里，也就搜不到，结果面板上要如实说清楚。
    QPushButton* headerSearchButton_ = nullptr;
    QWidget* appTopBar_ = nullptr;
    QListWidget* globalSearchResults_ = nullptr;
    // 搜索要扫所有会话，不能挂在每次按键上跑：停顿后才执行，组词期间不跑。
    QTimer* globalSearchUpdateTimer_ = nullptr;
    // 上一趟搜索里「有命中」的会话。会话列表过滤直接复用，避免同一次输入扫两遍。
    QSet<QString> peersWithSearchHits_;
    QString messageSearchHighlightedId_;
    bool renderedEmptyView_ = false;
    QPushButton* addContactButton_ = nullptr;
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
    // 图片预览只允许存在一个实例，避免重复点击或平台窗口事件重入后叠出多个预览。
    ImagePreviewDialog* imagePreviewDialog_ = nullptr;
    // 缩放百分比提示浮层（飞书式居中黑色气泡），零点几秒后自动隐藏。
    QLabel* zoomToast_ = nullptr;
    QTimer* zoomToastTimer_ = nullptr;
};
