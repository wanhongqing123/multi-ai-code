#pragma once

#include <QDialog>
#include <QLineEdit>
#include <QString>

// 与应用视觉统一的文本输入弹窗。
//
// 存在的理由：QInputDialog::getText 弹的是系统原生样式——标题栏带「?」按钮、
// 按钮是英文 OK/Cancel、输入框只有一条下划线，摆在这套界面里格格不入。
// 这里沿用 RemoteDesktopConsentDialog 的无边框圆角面板 + 中文按钮。
class AppTextInputDialog final : public QDialog {
    Q_OBJECT

public:
    struct Options {
        QString title;
        // 输入框上方的说明文字，可为空。
        QString description;
        QString placeholder;
        QString initialText;
        // 密码输入：回显为圆点。
        bool password = false;
        QString confirmText = QStringLiteral("确定");
        QString cancelText = QStringLiteral("取消");
    };

    explicit AppTextInputDialog(Options options, QWidget* parent = nullptr);

    QString text() const;

    // 便捷入口，替代 QInputDialog::getText。accepted 为 false 时返回值无意义。
    static QString getText(QWidget* parent, const Options& options, bool* accepted);

private:
    void buildUi(const Options& options);
    void applyStyle();

    QLineEdit* input_ = nullptr;
};
