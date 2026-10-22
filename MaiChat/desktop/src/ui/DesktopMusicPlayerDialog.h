// 文件作用：Desktop 本地音乐播放器；普通界面和 Agent 工具复用同一播放入口。
#pragma once

#include <QDialog>
#include <QString>

class QCloseEvent;
class QLabel;
class QPushButton;
class QSlider;
class QTimer;
struct MaiFfplayAudioSession;

class DesktopMusicPlayerDialog final : public QDialog {
public:
    // 验证用户选择的本地音频并打开唯一播放器；失败时返回 false 和原因。
    static bool openFile(QWidget* parent, const QString& path, QString* error);
    static void closeActive();

private:
    DesktopMusicPlayerDialog(QWidget* parent, const QString& path);
    ~DesktopMusicPlayerDialog() override;
    void closeEvent(QCloseEvent* event) override;
    void setFile(const QString& path);
    void stopPlayback();
    void refreshPlayback();

    MaiFfplayAudioSession* session_ = nullptr;
    QTimer* timer_ = nullptr;
    QString path_;
    bool playing_ = false;
    QLabel* title_ = nullptr;
    QLabel* time_ = nullptr;
    QLabel* status_ = nullptr;
    QSlider* progress_ = nullptr;
    QPushButton* play_ = nullptr;
};
