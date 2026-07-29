#include <QtTest/QtTest>

#include <QLabel>
#include <QPushButton>

#include "ui/AppTextInputDialog.h"

class AppTextInputDialogTest : public QObject {
    Q_OBJECT

private slots:
    void showsTitleAndDescriptionInsideThePanel();
    void masksPasswordInput();
    void confirmReturnsTextAndCancelDiscardsIt();
    void returnKeyConfirms();
};

void AppTextInputDialogTest::showsTitleAndDescriptionInsideThePanel() {
    AppTextInputDialog::Options options;
    options.title = QStringLiteral("访问密码");
    options.description = QStringLiteral("留空表示不设密码。");
    AppTextInputDialog dialog(options);

    // 标题必须画在面板内部：系统标题栏被去掉了（原生那条会带个「?」按钮，
    // 与这套界面不搭），标题只剩这一个落点，漏了就成了没头没脑的输入框。
    auto* title = dialog.findChild<QLabel*>(QStringLiteral("appTextInputTitle"));
    QVERIFY(title != nullptr);
    QCOMPARE(title->text(), QStringLiteral("访问密码"));

    auto* description = dialog.findChild<QLabel*>(QStringLiteral("appTextInputDescription"));
    QVERIFY(description != nullptr);
    QCOMPARE(description->text(), QStringLiteral("留空表示不设密码。"));

    // 按钮必须是中文，不能落回 Qt 默认的 OK/Cancel。
    auto* confirm = dialog.findChild<QPushButton*>(QStringLiteral("appTextInputConfirm"));
    auto* cancel = dialog.findChild<QPushButton*>(QStringLiteral("appTextInputCancel"));
    QVERIFY(confirm != nullptr && cancel != nullptr);
    QCOMPARE(confirm->text(), QStringLiteral("确定"));
    QCOMPARE(cancel->text(), QStringLiteral("取消"));
}

void AppTextInputDialogTest::masksPasswordInput() {
    AppTextInputDialog::Options options;
    options.title = QStringLiteral("访问密码");
    options.password = true;
    AppTextInputDialog dialog(options);

    auto* input = dialog.findChild<QLineEdit*>(QStringLiteral("appTextInput"));
    QVERIFY(input != nullptr);
    QCOMPARE(input->echoMode(), QLineEdit::Password);

    // 普通输入不该被打码。
    AppTextInputDialog::Options plain;
    plain.title = QStringLiteral("允许连入的设备");
    AppTextInputDialog plainDialog(plain);
    QCOMPARE(plainDialog.findChild<QLineEdit*>(QStringLiteral("appTextInput"))->echoMode(),
             QLineEdit::Normal);
}

void AppTextInputDialogTest::confirmReturnsTextAndCancelDiscardsIt() {
    AppTextInputDialog::Options options;
    options.title = QStringLiteral("允许连入的设备");
    options.initialText = QStringLiteral("whq-iphone");

    AppTextInputDialog dialog(options);
    auto* input = dialog.findChild<QLineEdit*>(QStringLiteral("appTextInput"));
    QCOMPARE(input->text(), QStringLiteral("whq-iphone"));

    input->setText(QStringLiteral("mac-air"));
    QSignalSpy acceptedSpy(&dialog, &QDialog::accepted);
    dialog.findChild<QPushButton*>(QStringLiteral("appTextInputConfirm"))->click();
    QCOMPARE(acceptedSpy.count(), 1);
    QCOMPARE(dialog.text(), QStringLiteral("mac-air"));

    AppTextInputDialog cancelled(options);
    QSignalSpy rejectedSpy(&cancelled, &QDialog::rejected);
    cancelled.findChild<QPushButton*>(QStringLiteral("appTextInputCancel"))->click();
    QCOMPARE(rejectedSpy.count(), 1);
}

void AppTextInputDialogTest::returnKeyConfirms() {
    AppTextInputDialog::Options options;
    options.title = QStringLiteral("访问密码");
    AppTextInputDialog dialog(options);

    auto* input = dialog.findChild<QLineEdit*>(QStringLiteral("appTextInput"));
    input->setText(QStringLiteral("pw"));
    QSignalSpy acceptedSpy(&dialog, &QDialog::accepted);
    // 输入完直接回车是最自然的动作，不该逼用户去点按钮。
    QTest::keyClick(input, Qt::Key_Return);
    QCOMPARE(acceptedSpy.count(), 1);
}

QTEST_MAIN(AppTextInputDialogTest)
#include "AppTextInputDialogTest.moc"
