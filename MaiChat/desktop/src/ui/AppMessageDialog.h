#pragma once

#include <QDialog>
#include <QString>

// 与应用视觉统一的提示 / 确认弹窗。
//
// 存在的理由与 AppTextInputDialog 相同：QMessageBox 弹的是系统原生样式，
// 摆在这套界面里格格不入。这里沿用同一套无边框圆角面板 + 中文按钮。
class AppMessageDialog final : public QDialog {
    Q_OBJECT

public:
    enum class Kind {
        Info,     // 一般提示，只有一个「知道了」
        Warning,  // 出错/失败，标题带警示色
        Confirm   // 二选一，主按钮可标记为危险动作
    };

    struct Options {
        Kind kind = Kind::Info;
        QString title;
        QString body;
        // 仅 Confirm 使用。
        QString confirmText = QStringLiteral("确定");
        QString cancelText = QStringLiteral("取消");
        // 删除类操作：主按钮用红色，且默认焦点落在取消上，
        // 误按回车不至于直接删掉东西。
        bool destructive = false;
    };

    explicit AppMessageDialog(Options options, QWidget* parent = nullptr);

    // 提示类：显示并等待关闭。
    static void show(QWidget* parent, Kind kind, const QString& title, const QString& body);
    // 确认类：返回用户是否点了主按钮。
    static bool confirm(QWidget* parent, const QString& title, const QString& body,
                        const QString& confirmText, bool destructive);

private:
    void buildUi(const Options& options);
    void applyStyle();
};
