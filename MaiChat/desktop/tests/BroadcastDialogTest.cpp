#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QTest>
#include <QTextEdit>

#include "ui/BroadcastDialog.h"

namespace {

RemoteIMContact makeContact(const QString& userId, const QString& name, const QString& group) {
    return RemoteIMContact{userId, name, QString(), group};
}

QListWidget* recipientsOf(BroadcastDialog& dialog) {
    return dialog.findChild<QListWidget*>(QStringLiteral("broadcastRecipients"));
}

QListWidgetItem* rowNamed(QListWidget* list, const QString& label) {
    for (int row = 0; row < list->count(); ++row) {
        if (list->item(row)->text().trimmed() == label) return list->item(row);
    }
    return nullptr;
}

}  // namespace

class BroadcastDialogTest : public QObject {
    Q_OBJECT

private slots:
    void checkingAGroupHeaderSelectsEveryMember();
    void partiallySelectedGroupShowsAsPartiallyChecked();
    void filteringHidesRowsButKeepsWhatIsAlreadySelected();
    void sendStaysDisabledUntilThereAreRecipientsAndText();
    void actionButtonsHaveBreathingRoom();
    void forwardingSelectsExactlyOneContactWithoutMessageComposer();
};

void BroadcastDialogTest::checkingAGroupHeaderSelectsEveryMember() {
    const QList<RemoteIMContact> contacts{
        makeContact(QStringLiteral("alice"), QStringLiteral("Alice"), QStringLiteral("同事")),
        makeContact(QStringLiteral("bob"), QStringLiteral("Bob"), QStringLiteral("同事")),
        makeContact(QStringLiteral("carol"), QStringLiteral("Carol"), QString())};
    BroadcastDialog dialog(contacts, {QStringLiteral("同事")}, QString());
    QListWidget* list = recipientsOf(dialog);
    QVERIFY(list != nullptr);

    QListWidgetItem* header = rowNamed(list, QStringLiteral("同事（2）"));
    QVERIFY(header != nullptr);
    header->setCheckState(Qt::Checked);

    // 勾表头一次选中整组，这是群发最常用的动作；组外的人不受影响。
    QCOMPARE(dialog.selectedPeerIds(), QStringList({QStringLiteral("alice"), QStringLiteral("bob")}));
}

void BroadcastDialogTest::partiallySelectedGroupShowsAsPartiallyChecked() {
    const QList<RemoteIMContact> contacts{
        makeContact(QStringLiteral("alice"), QStringLiteral("Alice"), QStringLiteral("同事")),
        makeContact(QStringLiteral("bob"), QStringLiteral("Bob"), QStringLiteral("同事")),
        makeContact(QStringLiteral("carol"), QStringLiteral("Carol"), QString())};
    BroadcastDialog dialog(contacts, {QStringLiteral("同事")}, QString());
    QListWidget* list = recipientsOf(dialog);

    QListWidgetItem* header = rowNamed(list, QStringLiteral("同事（2）"));
    QListWidgetItem* alice = rowNamed(list, QStringLiteral("Alice"));
    QVERIFY(header != nullptr && alice != nullptr);

    alice->setCheckState(Qt::Checked);
    // 半选状态是「这组没勾全」的唯一提示。显示成全选会让人以为整组都发到了。
    QCOMPARE(header->checkState(), Qt::PartiallyChecked);

    rowNamed(list, QStringLiteral("Bob"))->setCheckState(Qt::Checked);
    QCOMPARE(header->checkState(), Qt::Checked);

    alice->setCheckState(Qt::Unchecked);
    QCOMPARE(header->checkState(), Qt::PartiallyChecked);

    rowNamed(list, QStringLiteral("Bob"))->setCheckState(Qt::Unchecked);
    QCOMPARE(header->checkState(), Qt::Unchecked);

    // 组外的人不参与本组统计——否则勾一个散人就会让分组显示成半选。
    rowNamed(list, QStringLiteral("Carol"))->setCheckState(Qt::Checked);
    QCOMPARE(header->checkState(), Qt::Unchecked);
}

void BroadcastDialogTest::filteringHidesRowsButKeepsWhatIsAlreadySelected() {
    const QList<RemoteIMContact> contacts{
        makeContact(QStringLiteral("alice"), QStringLiteral("Alice"), QStringLiteral("同事")),
        makeContact(QStringLiteral("bob"), QStringLiteral("Bob"), QString())};
    BroadcastDialog dialog(contacts, {QStringLiteral("同事")}, QString());
    QListWidget* list = recipientsOf(dialog);
    auto* filter = dialog.findChild<QLineEdit*>(QStringLiteral("broadcastFilter"));
    QVERIFY(filter != nullptr);

    rowNamed(list, QStringLiteral("Alice"))->setCheckState(Qt::Checked);
    QCOMPARE(dialog.selectedPeerIds(), QStringList({QStringLiteral("alice")}));

    // 筛掉已勾选的那个人。筛选只改可见性——被筛掉的人如果悄悄取消勾选，
    // 用户清空筛选框之后会发现刚才勾的人没了，而且不会有任何提示。
    filter->setText(QStringLiteral("Bob"));
    QVERIFY(rowNamed(list, QStringLiteral("Alice"))->isHidden());
    QVERIFY(!rowNamed(list, QStringLiteral("Bob"))->isHidden());
    QCOMPARE(dialog.selectedPeerIds(), QStringList({QStringLiteral("alice")}));

    filter->clear();
    QVERIFY(!rowNamed(list, QStringLiteral("Alice"))->isHidden());
    QCOMPARE(rowNamed(list, QStringLiteral("Alice"))->checkState(), Qt::Checked);
    QCOMPARE(dialog.selectedPeerIds(), QStringList({QStringLiteral("alice")}));
}

void BroadcastDialogTest::sendStaysDisabledUntilThereAreRecipientsAndText() {
    const QList<RemoteIMContact> contacts{
        makeContact(QStringLiteral("alice"), QStringLiteral("Alice"), QString())};
    BroadcastDialog dialog(contacts, {}, QString());
    auto* send = dialog.findChild<QPushButton*>(QStringLiteral("broadcastSend"));
    auto* message = dialog.findChild<QTextEdit*>(QStringLiteral("broadcastMessage"));
    QVERIFY(send != nullptr && message != nullptr);

    QVERIFY(!send->isEnabled());

    message->setPlainText(QStringLiteral("发版了"));
    // 有正文但没选人，仍然发不出去。
    QVERIFY(!send->isEnabled());

    QListWidget* list = recipientsOf(dialog);
    rowNamed(list, QStringLiteral("Alice"))->setCheckState(Qt::Checked);
    QVERIFY(send->isEnabled());
    QCOMPARE(send->text(), QStringLiteral("发送给 1 人"));

    // 只剩空白的正文等于没有正文，不能让它发出去。
    message->setPlainText(QStringLiteral("   "));
    QVERIFY(!send->isEnabled());
}

void BroadcastDialogTest::actionButtonsHaveBreathingRoom() {
    const QList<RemoteIMContact> contacts{
        makeContact(QStringLiteral("alice"), QStringLiteral("Alice"), QString())};
    BroadcastDialog dialog(contacts, {}, QString());
    dialog.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));

    auto* cancel = dialog.findChild<QPushButton*>(QStringLiteral("broadcastCancel"));
    auto* send = dialog.findChild<QPushButton*>(QStringLiteral("broadcastSend"));
    QVERIFY(cancel != nullptr && send != nullptr);
    const int gap = send->mapTo(&dialog, QPoint(0, 0)).x()
        - cancel->mapTo(&dialog, QPoint(cancel->width(), 0)).x();
    QVERIFY2(gap >= 16, "群发窗口的取消与发送按钮仍然挤在一起");
    QVERIFY(cancel->width() >= 104);
    QVERIFY(send->width() >= 116);
}

void BroadcastDialogTest::forwardingSelectsExactlyOneContactWithoutMessageComposer() {
    const QList<RemoteIMContact> contacts{
        makeContact(QStringLiteral("alice"), QStringLiteral("Alice"), QStringLiteral("同事")),
        makeContact(QStringLiteral("bob"), QStringLiteral("Bob"), QStringLiteral("同事"))};
    BroadcastDialog dialog(contacts, {QStringLiteral("同事")}, QString(),
                           BroadcastDialog::Mode::Forward);
    QListWidget* list = recipientsOf(dialog);
    auto* send = dialog.findChild<QPushButton*>(QStringLiteral("broadcastSend"));
    QVERIFY(list != nullptr && send != nullptr);
    QVERIFY(dialog.findChild<QTextEdit*>(QStringLiteral("broadcastMessage")) == nullptr);
    // 转发是选一个会话，不显示会造成整组误发的分组表头。
    QCOMPARE(list->count(), 2);

    rowNamed(list, QStringLiteral("Alice"))->setCheckState(Qt::Checked);
    QVERIFY(send->isEnabled());
    QCOMPARE(dialog.selectedPeerIds(), QStringList({QStringLiteral("alice")}));

    rowNamed(list, QStringLiteral("Bob"))->setCheckState(Qt::Checked);
    QCOMPARE(dialog.selectedPeerIds(), QStringList({QStringLiteral("bob")}));
    QCOMPARE(send->text(), QStringLiteral("转发给 1 人"));
}

QTEST_MAIN(BroadcastDialogTest)
#include "BroadcastDialogTest.moc"
