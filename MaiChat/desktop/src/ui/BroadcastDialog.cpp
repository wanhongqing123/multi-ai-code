#include "ui/BroadcastDialog.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QTextEdit>
#include <QVBoxLayout>

#include "model/MessageSearch.h"
#include "ui/UiZoom.h"

namespace {

// 收件人列表里表头和联系人共用一个 QListWidget，和通讯录页同一套做法。
constexpr int IsGroupHeaderRole = Qt::UserRole + 1;
constexpr int PeerIdRole = Qt::UserRole + 2;
constexpr int GroupNameRole = Qt::UserRole + 3;

}  // namespace

BroadcastDialog::BroadcastDialog(const QList<RemoteIMContact>& contacts,
                                 const QStringList& groups,
                                 const QString& preselectedGroup,
                                 QWidget* parent)
    : QDialog(parent) {
    buildUi(contacts, groups, preselectedGroup);
    applyStyle();
}

QStringList BroadcastDialog::selectedPeerIds() const {
    QStringList selected;
    for (int row = 0; row < recipientList_->count(); ++row) {
        QListWidgetItem* item = recipientList_->item(row);
        if (item->data(IsGroupHeaderRole).toBool()) continue;
        if (item->checkState() != Qt::Checked) continue;
        selected.append(item->data(PeerIdRole).toString());
    }
    return selected;
}

QString BroadcastDialog::messageText() const {
    return messageInput_->toPlainText();
}

void BroadcastDialog::buildUi(const QList<RemoteIMContact>& contacts,
                              const QStringList& groups,
                              const QString& preselectedGroup) {
    setObjectName(QStringLiteral("broadcastDialog"));
    setWindowTitle(QStringLiteral("群发消息"));
    setModal(true);
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground, true);

    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(18, 18, 18, 18);
    rootLayout->setSpacing(0);

    auto* panel = new QFrame(this);
    panel->setObjectName(QStringLiteral("broadcastPanel"));
    rootLayout->addWidget(panel);

    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(28, 26, 28, 22);
    layout->setSpacing(UiZoom::s(12));

    auto* title = new QLabel(QStringLiteral("群发消息"), panel);
    title->setObjectName(QStringLiteral("broadcastTitle"));
    layout->addWidget(title);

    auto* description = new QLabel(
        QStringLiteral("勾选的每个人都会单独收到一条消息，和平时的私聊没有区别。"),
        panel);
    description->setObjectName(QStringLiteral("broadcastDescription"));
    description->setWordWrap(true);
    layout->addWidget(description);

    filterInput_ = new QLineEdit(panel);
    filterInput_->setObjectName(QStringLiteral("broadcastFilter"));
    filterInput_->setPlaceholderText(QStringLiteral("筛选联系人"));
    filterInput_->setClearButtonEnabled(true);
    layout->addWidget(filterInput_);

    recipientList_ = new QListWidget(panel);
    recipientList_->setObjectName(QStringLiteral("broadcastRecipients"));
    recipientList_->setFrameShape(QFrame::NoFrame);
    recipientList_->setMinimumHeight(UiZoom::s(220));

    // 按分组归拢，和通讯录页看到的顺序一致：分组在前，没有分组的人跟在后面。
    // 顺序不一致会让人怀疑自己是不是漏勾了谁。
    QHash<QString, QList<RemoteIMContact>> byGroup;
    for (const RemoteIMContact& contact : contacts) byGroup[contact.groupName].append(contact);

    auto addContactRow = [this](const RemoteIMContact& contact, bool checked, bool indented) {
        auto* item = new QListWidgetItem(
            (indented ? QStringLiteral("    ") : QString())
            + (contact.displayName.isEmpty() ? contact.userId : contact.displayName));
        item->setData(IsGroupHeaderRole, false);
        item->setData(PeerIdRole, contact.userId);
        item->setData(GroupNameRole, contact.groupName);
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
        item->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
        recipientList_->addItem(item);
    };

    for (const QString& group : groups) {
        const QList<RemoteIMContact> members = byGroup.value(group);
        auto* header = new QListWidgetItem(QStringLiteral("%1（%2）").arg(group).arg(members.size()));
        header->setData(IsGroupHeaderRole, true);
        header->setData(GroupNameRole, group);
        // 分组表头本身可勾选：一次勾中整组，这是群发最常用的动作。
        header->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
        header->setCheckState(Qt::Unchecked);
        recipientList_->addItem(header);
        for (const RemoteIMContact& contact : members) {
            addContactRow(contact, group == preselectedGroup, true);
        }
    }
    for (const RemoteIMContact& contact : byGroup.value(QString())) {
        addContactRow(contact, false, false);
    }
    layout->addWidget(recipientList_, 1);

    messageInput_ = new QTextEdit(panel);
    messageInput_->setObjectName(QStringLiteral("broadcastMessage"));
    messageInput_->setPlaceholderText(QStringLiteral("要发送的内容"));
    messageInput_->setMinimumHeight(UiZoom::s(90));
    layout->addWidget(messageInput_);

    auto* buttonRow = new QHBoxLayout();
    buttonRow->setContentsMargins(0, 0, 0, 0);
    buttonRow->setSpacing(UiZoom::s(10));

    summaryLabel_ = new QLabel(panel);
    summaryLabel_->setObjectName(QStringLiteral("broadcastSummary"));
    buttonRow->addWidget(summaryLabel_);
    buttonRow->addStretch(1);

    auto* cancel = new QPushButton(QStringLiteral("取消"), panel);
    cancel->setObjectName(QStringLiteral("broadcastCancel"));
    cancel->setCursor(Qt::PointingHandCursor);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);

    sendButton_ = new QPushButton(panel);
    sendButton_->setObjectName(QStringLiteral("broadcastSend"));
    sendButton_->setCursor(Qt::PointingHandCursor);
    sendButton_->setDefault(true);
    connect(sendButton_, &QPushButton::clicked, this, &QDialog::accept);

    buttonRow->addWidget(cancel);
    buttonRow->addWidget(sendButton_);
    layout->addLayout(buttonRow);

    connect(messageInput_, &QTextEdit::textChanged, this, [this] { refreshSendState(); });
    connect(recipientList_, &QListWidget::itemChanged, this, [this](QListWidgetItem* item) {
        if (syncingChecks_) return;
        if (item->data(IsGroupHeaderRole).toBool()) {
            toggleGroupSelection(item);
        } else {
            syncGroupCheckStates();
        }
        refreshSendState();
    });
    connect(filterInput_, &QLineEdit::textChanged, this, [this](const QString& needle) {
        const QString clean = needle.trimmed();
        for (int row = 0; row < recipientList_->count(); ++row) {
            QListWidgetItem* item = recipientList_->item(row);
            // 表头跟着筛选一起藏，否则会剩下一堆空标题。
            // 勾选状态不受筛选影响：筛掉的人如果被悄悄取消勾选，
            // 用户清空筛选框之后会发现刚才勾的人没了。
            const bool matched = clean.isEmpty() || MessageSearch::matches(item->text(), clean);
            item->setHidden(!matched);
        }
    });

    syncGroupCheckStates();
    refreshSendState();
    setMinimumWidth(UiZoom::s(520));
    messageInput_->setFocus(Qt::OtherFocusReason);
}

void BroadcastDialog::toggleGroupSelection(QListWidgetItem* header) {
    const Qt::CheckState target =
        header->checkState() == Qt::Checked ? Qt::Checked : Qt::Unchecked;
    const QString group = header->data(GroupNameRole).toString();
    syncingChecks_ = true;
    for (int row = 0; row < recipientList_->count(); ++row) {
        QListWidgetItem* item = recipientList_->item(row);
        if (item->data(IsGroupHeaderRole).toBool()) continue;
        if (item->data(GroupNameRole).toString() != group) continue;
        item->setCheckState(target);
    }
    syncingChecks_ = false;
}

void BroadcastDialog::syncGroupCheckStates() {
    syncingChecks_ = true;
    QListWidgetItem* header = nullptr;
    int total = 0;
    int checked = 0;
    auto closeSection = [&] {
        if (!header) return;
        // 部分选中显示成 PartiallyChecked，用户一眼能看出这组没勾全。
        header->setCheckState(total == 0 || checked == 0 ? Qt::Unchecked
                              : checked == total         ? Qt::Checked
                                                         : Qt::PartiallyChecked);
        header = nullptr;
        total = 0;
        checked = 0;
    };
    for (int row = 0; row < recipientList_->count(); ++row) {
        QListWidgetItem* item = recipientList_->item(row);
        if (item->data(IsGroupHeaderRole).toBool()) {
            closeSection();
            header = item;
            continue;
        }
        // 没有分组的人不属于任何一节，不能算进上一个分组的统计。
        if (item->data(GroupNameRole).toString().isEmpty()) {
            closeSection();
            continue;
        }
        ++total;
        if (item->checkState() == Qt::Checked) ++checked;
    }
    closeSection();
    syncingChecks_ = false;
}

void BroadcastDialog::refreshSendState() {
    const int count = selectedPeerIds().size();
    const bool hasText = !messageInput_->toPlainText().trimmed().isEmpty();
    sendButton_->setText(count > 0 ? QStringLiteral("发送给 %1 人").arg(count)
                                   : QStringLiteral("发送"));
    sendButton_->setEnabled(count > 0 && hasText);
    summaryLabel_->setText(count > 0 ? QStringLiteral("已选 %1 人").arg(count)
                                     : QStringLiteral("还没有选人"));
}

void BroadcastDialog::applyStyle() {
    setStyleSheet(UiZoom::scaleQss(QStringLiteral(R"(
        #broadcastPanel {
            background: #ffffff;
            border-radius: 16px;
        }
        #broadcastTitle {
            color: #0f172a;
            font-size: 18px;
            font-weight: 800;
        }
        #broadcastDescription {
            color: #667085;
            font-size: 13px;
        }
        #broadcastSummary {
            color: #667085;
            font-size: 13px;
        }
        #broadcastFilter, #broadcastMessage {
            background: #f8fafc;
            border: 1px solid #d9e1ec;
            border-radius: 10px;
            color: #172033;
            font-size: 14px;
            padding: 8px 12px;
        }
        #broadcastFilter:focus, #broadcastMessage:focus {
            background: #ffffff;
            border-color: #5b9bff;
        }
        #broadcastRecipients {
            background: #f8fafc;
            border: 1px solid #d9e1ec;
            border-radius: 10px;
            color: #172033;
            font-size: 14px;
            padding: 4px;
        }
        #broadcastRecipients::item {
            padding: 6px 4px;
        }
        #broadcastCancel, #broadcastSend {
            border-radius: 10px;
            font-size: 14px;
            font-weight: 700;
            padding: 10px 22px;
        }
        #broadcastCancel {
            background: #f1f5f9;
            border: 1px solid #d9e1ec;
            color: #334155;
        }
        #broadcastCancel:hover {
            background: #e2e8f0;
        }
        #broadcastSend {
            background: qlineargradient(x1:0, y1:0, x2:1, y2:1,
                                        stop:0 #5b9bff, stop:1 #1e40af);
            border: 0;
            color: #ffffff;
        }
        #broadcastSend:disabled {
            background: #cbd5e1;
            color: #f8fafc;
        }
    )")));
}
