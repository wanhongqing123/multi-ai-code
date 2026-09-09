#include "ui/RemoteDiagnosticsDialog.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPushButton>
#include <QVBoxLayout>

#include "ui/UiZoom.h"

namespace {

constexpr int PeerIdRole = Qt::UserRole + 2;

}  // namespace

RemoteDiagnosticsDialog::RemoteDiagnosticsDialog(const QString& faultyPeerName,
                                                 const QList<RemoteIMContact>& candidates,
                                                 QWidget* parent)
    : QDialog(parent), faultyPeerName_(faultyPeerName) {
    buildUi(faultyPeerName, candidates);
    applyStyle();
    refreshConfirmState();
}

QString RemoteDiagnosticsDialog::selectedRecipientId() const {
    QListWidgetItem* item = recipientList_->currentItem();
    return item ? item->data(PeerIdRole).toString() : QString();
}

void RemoteDiagnosticsDialog::buildUi(const QString& faultyPeerName,
                                      const QList<RemoteIMContact>& candidates) {
    setObjectName(QStringLiteral("remoteDiagnosticsDialog"));
    setWindowTitle(QStringLiteral("远程排障"));
    setModal(true);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(UiZoom::s(24), UiZoom::s(20), UiZoom::s(24), UiZoom::s(20));
    layout->setSpacing(UiZoom::s(12));

    // 先把「要做什么」说清楚，再让人选收件人。顺序反过来的话，
    // 用户会先挑人、后才读到内容说明。
    auto* intro = new QLabel(this);
    intro->setObjectName(QStringLiteral("diagnosticsIntro"));
    intro->setWordWrap(true);
    intro->setText(QStringLiteral(
        "向 %1 请求一份排障报告，和本机现场合并后发给你选择的好友。")
                       .arg(faultyPeerName.isEmpty() ? QStringLiteral("对方") : faultyPeerName));
    layout->addWidget(intro);

    auto* contents = new QLabel(this);
    contents->setObjectName(QStringLiteral("diagnosticsContents"));
    contents->setWordWrap(true);
    // 逐条写明范围。含糊的「诊断信息」等于没说，用户没法判断该不该发。
    contents->setText(QStringLiteral(
        "报告只包含设备与会话的元数据：应用版本、系统平台、消息编号与收发状态、"
        "附件下载各阶段的结果。\n"
        "不包含聊天正文、账号密码或登录凭据，也不包含文件路径与下载地址。"));
    layout->addWidget(contents);

    auto* pick = new QLabel(QStringLiteral("把报告发给："), this);
    pick->setObjectName(QStringLiteral("diagnosticsPickLabel"));
    layout->addWidget(pick);

    filterInput_ = new QLineEdit(this);
    filterInput_->setObjectName(QStringLiteral("diagnosticsFilter"));
    filterInput_->setPlaceholderText(QStringLiteral("搜索好友"));
    layout->addWidget(filterInput_);

    recipientList_ = new QListWidget(this);
    recipientList_->setObjectName(QStringLiteral("diagnosticsRecipients"));
    recipientList_->setSelectionMode(QAbstractItemView::SingleSelection);
    for (const RemoteIMContact& contact : candidates) {
        const QString label = contact.displayName.isEmpty() || contact.displayName == contact.userId
            ? contact.userId : QStringLiteral("%1 (%2)").arg(contact.displayName, contact.userId);
        auto* item = new QListWidgetItem(label, recipientList_);
        item->setData(PeerIdRole, contact.userId);
    }
    layout->addWidget(recipientList_, /*stretch*/ 1);

    summaryLabel_ = new QLabel(this);
    summaryLabel_->setObjectName(QStringLiteral("diagnosticsSummary"));
    summaryLabel_->setWordWrap(true);
    layout->addWidget(summaryLabel_);

    auto* buttons = new QHBoxLayout();
    buttons->addStretch(1);
    auto* cancel = new QPushButton(QStringLiteral("取消"), this);
    cancel->setObjectName(QStringLiteral("diagnosticsCancel"));
    confirmButton_ = new QPushButton(QStringLiteral("开始收集"), this);
    confirmButton_->setObjectName(QStringLiteral("diagnosticsConfirm"));
    confirmButton_->setDefault(true);
    buttons->addWidget(cancel);
    buttons->addWidget(confirmButton_);
    layout->addLayout(buttons);

    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(confirmButton_, &QPushButton::clicked, this, &QDialog::accept);
    connect(recipientList_, &QListWidget::currentItemChanged, this,
            [this] { refreshConfirmState(); });
    connect(filterInput_, &QLineEdit::textChanged, this, [this](const QString& text) {
        for (int row = 0; row < recipientList_->count(); ++row) {
            QListWidgetItem* item = recipientList_->item(row);
            const bool match =
                text.trimmed().isEmpty() || item->text().contains(text.trimmed(), Qt::CaseInsensitive);
            item->setHidden(!match);
            // 过滤掉的项如果还选着，确认按钮会显示一个看不见的收件人。
            if (!match && item->isSelected()) item->setSelected(false);
        }
        refreshConfirmState();
    });
}

void RemoteDiagnosticsDialog::refreshConfirmState() {
    QListWidgetItem* item = recipientList_->currentItem();
    const bool usable = item && !item->isHidden();
    confirmButton_->setEnabled(usable);
    // 确认前把收件人**再说一遍**：这是发给第三个人的动作，
    // 选错人的代价是把设备信息发给了不该看的人。
    summaryLabel_->setText(
        usable ? QStringLiteral("确认后将发送给：%1").arg(item->text())
               : QStringLiteral("请选择一位接收报告的好友。"));
}

void RemoteDiagnosticsDialog::applyStyle() {
    setStyleSheet(UiZoom::scaleQss(QStringLiteral(R"(
        #remoteDiagnosticsDialog { background: #ffffff; }
        #diagnosticsIntro { font-size: 15px; color: #1f2329; }
        #diagnosticsContents { font-size: 13px; color: #646a73; }
        #diagnosticsPickLabel { font-size: 13px; color: #1f2329; }
        #diagnosticsFilter { font-size: 13px; padding: 6px 10px; border: 1px solid #dee0e3;
                             border-radius: 6px; }
        #diagnosticsRecipients { font-size: 13px; border: 1px solid #dee0e3; border-radius: 6px; }
        #diagnosticsSummary { font-size: 12px; color: #646a73; }
        #diagnosticsCancel, #diagnosticsConfirm { font-size: 13px; padding: 6px 18px;
                                                  border-radius: 6px; }
        #diagnosticsConfirm { background: #3370ff; color: #ffffff; border: none; }
        #diagnosticsConfirm:disabled { background: #bcd0ff; }
    )")));
    resize(UiZoom::s(420), UiZoom::s(460));
}
