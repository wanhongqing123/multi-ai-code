#pragma once

#include <QDialog>
#include <QList>
#include <QString>

#include "model/RemoteIMContact.h"

class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;

// 远程排障的确认框：选一个接收报告的好友（C），并**在确认前写清楚**
// 会发生什么、发给谁、包含什么。
//
// 为什么必须写清楚：这个动作会把一份含设备与会话元数据的报告发给第三个人。
// 用户点之前必须知道收件人是谁、内容是什么范围，事后再解释就晚了。
//
// C 在这里选定之后由控制器锁住，**不随聊天窗口切换而变**——
// 用户确认的是「发给张三」，不是「发给我当时正好开着的那个会话」。
class RemoteDiagnosticsDialog final : public QDialog {
    Q_OBJECT

public:
    RemoteDiagnosticsDialog(const QString& faultyPeerName,
                            const QList<RemoteIMContact>& candidates,
                            QWidget* parent = nullptr);

    // 选中的接收人（C）。未选时为空，此时对话框不允许确认。
    QString selectedRecipientId() const;

private:
    void buildUi(const QString& faultyPeerName, const QList<RemoteIMContact>& candidates);
    void applyStyle();
    void refreshConfirmState();

    QLineEdit* filterInput_ = nullptr;
    QListWidget* recipientList_ = nullptr;
    QPushButton* confirmButton_ = nullptr;
    QLabel* summaryLabel_ = nullptr;
    QString faultyPeerName_;
};
