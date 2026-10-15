// 文件作用：桌面端共用界面；RemoteDesktopProxyDialog 负责对话框呈现、确认交互和结果回传，本文件给出其声明。
// 类型关系：RemoteDesktopProxyDialog final : public QDialog；审阅时可据此追踪父类、接口或回调契约。
// 对外边界：本文件给出 RemoteDesktopProxyDialog 的类型和函数声明；参数、返回值和对象生命周期以此为准。
#pragma once

#include <QDialog>
#include <QString>

class QCheckBox;
class QLabel;
class QLineEdit;
class QSpinBox;

class RemoteDesktopProxyDialog final : public QDialog {
public:
    struct Config {
        bool enabled = false;
        QString host = QStringLiteral("127.0.0.1");
        quint16 port = 1082;
        bool supportUdp = false;
    };

    explicit RemoteDesktopProxyDialog(Config config, QWidget* parent = nullptr);

    Config config() const;

private:
    void buildUi(const Config& config);
    void applyStyle();
    void updateFieldState();
    void validateAndAccept();

    QCheckBox* enabled_ = nullptr;
    QLineEdit* host_ = nullptr;
    QSpinBox* port_ = nullptr;
    QCheckBox* udp_ = nullptr;
    QLabel* error_ = nullptr;
};
