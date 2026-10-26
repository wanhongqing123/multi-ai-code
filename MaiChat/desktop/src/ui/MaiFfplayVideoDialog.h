// 文件作用：桌面端共用界面；MaiFfplayVideoDialog 负责对话框呈现、确认交互和结果回传，本文件给出其声明。
// 类型关系：MaiFfplayVideoDialog final : public QDialog；审阅时可据此追踪父类、接口或回调契约。
// 对外边界：本文件给出 MaiFfplayVideoDialog 的类型和函数声明；参数、返回值和对象生命周期以此为准。
// 公开入口：activeDialog、setTitle、setSize、setPosition。
// 直接依赖：MaiFfplayEntry.h；实现与这些接口协作。
#pragma once

#include <QDialog>
#include <QPointer>
#include <QString>
#include <QStringList>

#include <thread>
#include <atomic>
#include <functional>
#include <memory>

#include "MaiFfplayEntry.h"

class QPushButton;
class QComboBox;
class QSlider;
class QLabel;
class QTimer;
class QWidget;
class QEvent;
struct MaiCloudVod;

class MaiFfplayVideoDialog final : public QDialog {
    Q_OBJECT

public:
    explicit MaiFfplayVideoDialog(const QString& path, QWidget* parent = nullptr,
                                  int64_t startPositionUs = 0);
    MaiFfplayVideoDialog(const QString& displayName,
                         std::shared_ptr<MaiCloudVod> source,
                         QWidget* parent = nullptr);
    ~MaiFfplayVideoDialog() override;

    static QPointer<MaiFfplayVideoDialog> activeDialog();
    bool sendCommand(const QString& command);
    bool seekPercent(double fraction);
    bool seekSeconds(double seconds);
    bool setSpeed(double speed);
    bool replay();
    void setPlaylist(const QStringList& paths);
    bool navigateVideo(int direction);
    double speed() const { return playbackSpeed_; }
    bool isStarted() const { return started_; }
    bool isPlaying() const { return playing_; }
    uint64_t presentedFrames() const;
    void setForwardAction(std::function<void(const QString&)> action);
    void setDocked(bool docked) { docked_ = docked; }

protected:
    void showEvent(QShowEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    static bool presentVideo(void* userData, const MaiVideoFrame* frame,
                             const MaiVideoSubtitle* subtitle);
    static bool presentRgba(void* userData, const uint8_t* pixels,
                            uint32_t width, uint32_t height, uint32_t stride);
    static int readCloudInput(void* userData, uint64_t offset,
                              uint8_t* output, int capacity);
    static void setTitle(void* userData, const char* title);
    static void setSize(void* userData, int width, int height);
    static void setPosition(void* userData, int x, int y);
    static void setFullscreen(void* userData, int enabled);
    static void showWindow(void* userData);
    bool startPlayback();
    void stopPlayback(bool cancelCloudSource = false);
    void saveCopy();
    void forwardVideo();

    QString path_;
    std::shared_ptr<MaiCloudVod> cloudSource_;
    QStringList playlist_;
    int playlistIndex_ = 0;
    QWidget* surface_ = nullptr;
    QWidget* overlay_ = nullptr;
    QPushButton* pauseButton_ = nullptr;
    QPushButton* previousButton_ = nullptr;
    QPushButton* nextButton_ = nullptr;
    QComboBox* speedSelector_ = nullptr;
    QSlider* seekSlider_ = nullptr;
    QLabel* timeLabel_ = nullptr;
    QTimer* progressTimer_ = nullptr;
    QLabel* errorLabel_ = nullptr;
    MaiFfplayHost host_{};
    std::thread playbackThread_;
    void* nativeView_ = nullptr;
    uint64_t viewId_ = 0;
    bool started_ = false;
    bool playing_ = true;
    bool docked_ = false;
    double playbackSpeed_ = 1.0;
    int64_t startPositionUs_ = 0;
    std::atomic<bool> playbackFinished_{false};
    std::function<void(const QString&)> forwardAction_;
};
