#pragma once

#include <QDialog>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

#include "model/RemoteIMContact.h"

class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QTextEdit;

// 群发消息：勾选若干联系人（或整个分组），给每人各发一条独立消息。
//
// 不是「群聊」——收件人那边看到的就是一条普通私聊消息，各自的聊天记录也是完整的。
// 系统里没有群，硬做一个假的群会让历史记录变成另一套东西。
class BroadcastDialog final : public QDialog {
    Q_OBJECT

public:
    BroadcastDialog(const QList<RemoteIMContact>& contacts,
                    const QStringList& groups,
                    const QString& preselectedGroup,
                    QWidget* parent = nullptr);

    QStringList selectedPeerIds() const;
    QString messageText() const;

private:
    void buildUi(const QList<RemoteIMContact>& contacts,
                 const QStringList& groups,
                 const QString& preselectedGroup);
    void applyStyle();
    // 勾选数和正文一起决定发送按钮能不能点，两者任一为空都发不出去。
    void refreshSendState();
    // 分组表头的三态勾选：全选 / 全不选 / 部分选中。
    void syncGroupCheckStates();
    void toggleGroupSelection(QListWidgetItem* header);

    QLineEdit* filterInput_ = nullptr;
    QListWidget* recipientList_ = nullptr;
    QTextEdit* messageInput_ = nullptr;
    QPushButton* sendButton_ = nullptr;
    QLabel* summaryLabel_ = nullptr;
    bool syncingChecks_ = false;
};
