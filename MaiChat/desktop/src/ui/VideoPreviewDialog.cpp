#include "ui/VideoPreviewDialog.h"

#include <QFileInfo>
#include <QCloseEvent>
#include <QDebug>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMediaPlayer>
#include <QPushButton>
#include <QSlider>
#include <QUrl>
#include <QVBoxLayout>
#include <QVideoWidget>

#include "ui/UiZoom.h"

VideoPreviewDialog::VideoPreviewDialog(const QString& videoPath, const QString& title, QWidget* parent)
    : QDialog(parent), videoPath_(videoPath) {
    setWindowTitle(title.isEmpty() ? QStringLiteral("视频") : title);
    setMinimumSize(UiZoom::s(640), UiZoom::s(420));
    setStyleSheet(UiZoom::scaleQss(QStringLiteral(R"(
        QDialog { background: #10151f; }
        QLabel { color: #d9e4ef; font-size: 13px; }
        QLabel#videoError { color: #ffb4b4; font-size: 13px; }
        QPushButton {
            background: #1f2a3a; border: 1px solid #33475b; border-radius: 6px;
            color: #e8f0f8; font-size: 13px; padding: 6px 16px;
        }
        QPushButton:hover { background: #27354a; }
    )")));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);

    videoWidget_ = new QVideoWidget(this);
    videoWidget_->setMinimumHeight(UiZoom::s(320));
    layout->addWidget(videoWidget_, 1);

    errorLabel_ = new QLabel(this);
    errorLabel_->setObjectName(QStringLiteral("videoError"));
    errorLabel_->setWordWrap(true);
    errorLabel_->hide();
    layout->addWidget(errorLabel_);

    auto* controls = new QHBoxLayout();
    controls->setSpacing(10);
    playButton_ = new QPushButton(QStringLiteral("暂停"), this);
    positionSlider_ = new QSlider(Qt::Horizontal, this);
    positionSlider_->setRange(0, 0);
    positionLabel_ = new QLabel(QStringLiteral("00:00 / 00:00"), this);
    controls->addWidget(playButton_);
    controls->addWidget(positionSlider_, 1);
    controls->addWidget(positionLabel_);
    layout->addLayout(controls);

    player_ = new QMediaPlayer(this);
    player_->setVideoOutput(videoWidget_);

    connect(playButton_, &QPushButton::clicked, this, &VideoPreviewDialog::togglePlayback);

    // 拖动进度条时不要被 positionChanged 反向覆盖，否则滑块会跳回去。
    connect(positionSlider_, &QSlider::sliderPressed, this, [this]() { sliderPressed_ = true; });
    connect(positionSlider_, &QSlider::sliderReleased, this, [this]() {
        sliderPressed_ = false;
        if (player_) player_->setPosition(positionSlider_->value());
    });

    connect(player_, &QMediaPlayer::durationChanged, this, [this](qint64 duration) {
        positionSlider_->setRange(0, static_cast<int>(duration));
    });
    connect(player_, &QMediaPlayer::positionChanged, this, [this](qint64 position) {
        if (!sliderPressed_) positionSlider_->setValue(static_cast<int>(position));
        positionLabel_->setText(formatDuration(position) + QStringLiteral(" / ")
                                + formatDuration(player_->duration()));
    });
    connect(player_, &QMediaPlayer::stateChanged, this, [this](QMediaPlayer::State state) {
        playButton_->setText(state == QMediaPlayer::PlayingState ? QStringLiteral("暂停")
                                                                 : QStringLiteral("播放"));
    });
    // Qt5 里 error 是重载信号，必须显式取地址消歧义。
    connect(player_, QOverload<QMediaPlayer::Error>::of(&QMediaPlayer::error), this,
            [this](QMediaPlayer::Error) { showError(player_->errorString()); });

    const QFileInfo info(videoPath_);
    if (!info.isFile()) {
        showError(QStringLiteral("视频文件不存在或已被清理：%1").arg(videoPath_));
        playButton_->setEnabled(false);
        return;
    }
    player_->setMedia(QUrl::fromLocalFile(info.absoluteFilePath()));
    player_->play();
}

VideoPreviewDialog::~VideoPreviewDialog() {
    // 正常关闭会在 closeEvent（原生窗口/CALayer 仍有效）里完成播放器销毁。
    // 父窗口直接析构等少数路径没有 closeEvent，此处只做无 stop 的兜底释放：
    // macOS AVFoundation 后端在原生视图已关闭后调用 stop() 会访问悬空 CALayer。
    shutdownPlayer(false, "destructor-fallback");
}

void VideoPreviewDialog::closeEvent(QCloseEvent* event) {
    // WA_DeleteOnClose 会先关闭原生窗口，再投递 DeferredDelete。原实现把 stop()
    // 放在析构函数里，执行时 NSView/CALayer 已失效，libqavfmediaplayer 会在
    // objc_msgSend 处 EXC_BAD_ACCESS。必须在交给 QDialog 关闭窗口之前拆播放器。
    shutdownPlayer(true, "close-event");
    QDialog::closeEvent(event);
}

void VideoPreviewDialog::shutdownPlayer(bool stopPlayback, const char* reason) {
    if (!player_) return;
    QMediaPlayer* player = player_;
    player_ = nullptr;
    const QMediaPlayer::State state = player->state();
    qInfo().noquote()
        << QStringLiteral("[video-preview] teardown begin reason=%1 state=%2 file=%3")
               .arg(QString::fromLatin1(reason))
               .arg(static_cast<int>(state))
               .arg(QFileInfo(videoPath_).fileName());

    // 防止 stop/detach 期间的同步状态信号回调已经在关闭中的控件。
    disconnect(player, nullptr, this, nullptr);
    if (stopPlayback && state != QMediaPlayer::StoppedState) player->stop();
    player->setVideoOutput(static_cast<QVideoWidget*>(nullptr));
    delete player;
    qInfo().noquote()
        << QStringLiteral("[video-preview] teardown complete reason=%1")
               .arg(QString::fromLatin1(reason));
}

void VideoPreviewDialog::togglePlayback() {
    if (!player_) return;
    if (player_->state() == QMediaPlayer::PlayingState) {
        player_->pause();
    } else {
        player_->play();
    }
}

void VideoPreviewDialog::showError(const QString& message) {
    if (!errorLabel_) return;
    const QString detail = message.trimmed().isEmpty() ? QStringLiteral("未知错误") : message.trimmed();
    // 不要笼统地说「缺插件」：实测插件齐全时也会失败——Qt 挑中了 DirectShow 后端，
    // 而它解不了 H.264（0x80040266 VFW_E_UNSUPPORTED_STREAM）。把当前首选后端一并
    // 显示出来，才能一眼分清是「没插件」还是「挑错了后端」。
    const QString backend = qEnvironmentVariable("QT_MULTIMEDIA_PREFERRED_PLUGINS",
                                                 QStringLiteral("(系统默认)"));
    errorLabel_->setText(
        QStringLiteral("无法播放：%1\n首选解码后端：%2")
            .arg(detail, backend));
    errorLabel_->show();
}

QString VideoPreviewDialog::formatDuration(qint64 milliseconds) {
    if (milliseconds < 0) milliseconds = 0;
    const qint64 totalSeconds = milliseconds / 1000;
    const qint64 minutes = totalSeconds / 60;
    const qint64 seconds = totalSeconds % 60;
    return QStringLiteral("%1:%2")
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(seconds, 2, 10, QLatin1Char('0'));
}

void VideoPreviewDialog::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        close();
        return;
    }
    if (event->key() == Qt::Key_Space) {
        togglePlayback();
        return;
    }
    QDialog::keyPressEvent(event);
}
