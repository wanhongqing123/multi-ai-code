#pragma once

#include <QDialog>
#include <QString>

class QLabel;
class QCloseEvent;
class QMediaPlayer;
class QPushButton;
class QSlider;
class QVideoWidget;

// 视频播放窗。
//
// Qt5 的 QMediaPlayer 解码走插件（Windows 上是 plugins/mediaservice 里的
// wmfengine/dsengine）。插件缺失时它**不会抛异常也不会崩**，只是永远停在
// StoppedState、画面全黑——这正是打包后最容易出现、也最难查的那类故障。
// 所以这里把 error 信号接出来显式呈现，而不是让用户对着一块黑屏猜。
class VideoPreviewDialog final : public QDialog {
    Q_OBJECT

public:
    explicit VideoPreviewDialog(const QString& videoPath, const QString& title, QWidget* parent = nullptr);
    ~VideoPreviewDialog() override;

protected:
    void closeEvent(QCloseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    void togglePlayback();
    void shutdownPlayer(bool stopPlayback, const char* reason);
    void showError(const QString& message);
    static QString formatDuration(qint64 milliseconds);

    QString videoPath_;
    QMediaPlayer* player_ = nullptr;
    QVideoWidget* videoWidget_ = nullptr;
    QPushButton* playButton_ = nullptr;
    QSlider* positionSlider_ = nullptr;
    QLabel* positionLabel_ = nullptr;
    QLabel* errorLabel_ = nullptr;
    bool sliderPressed_ = false;
};
