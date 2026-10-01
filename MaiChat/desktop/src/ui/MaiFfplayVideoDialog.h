#pragma once

#include <QDialog>
#include <QPointer>
#include <QString>

#include <thread>
#include <atomic>

#include "MaiFfplayEntry.h"

class QPushButton;
class QSlider;
class QLabel;
class QTimer;
class QWidget;

class MaiFfplayVideoDialog final : public QDialog {
    Q_OBJECT

public:
    explicit MaiFfplayVideoDialog(const QString& path, QWidget* parent = nullptr);
    ~MaiFfplayVideoDialog() override;

    static QPointer<MaiFfplayVideoDialog> activeDialog();
    bool sendCommand(const QString& command);
    bool seekPercent(double fraction);
    bool isStarted() const { return started_; }
    bool isPlaying() const { return playing_; }
    uint64_t presentedFrames() const;

protected:
    void showEvent(QShowEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    static bool presentVideo(void* userData, const MaiVideoFrame* frame,
                             const MaiVideoSubtitle* subtitle);
    static bool presentRgba(void* userData, const uint8_t* pixels,
                            uint32_t width, uint32_t height, uint32_t stride);
    static void setTitle(void* userData, const char* title);
    static void setSize(void* userData, int width, int height);
    static void setPosition(void* userData, int x, int y);
    static void setFullscreen(void* userData, int enabled);
    static void showWindow(void* userData);
    bool startPlayback();
    void stopPlayback();

    QString path_;
    QWidget* surface_ = nullptr;
    QPushButton* pauseButton_ = nullptr;
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
    std::atomic<bool> playbackFinished_{false};
};
