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
