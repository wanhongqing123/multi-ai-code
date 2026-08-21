#include "ui/VideoPreviewDialog.h"

#include <QFileInfo>
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
        player_->setPosition(positionSlider_->value());
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
    // 不停就析构的话，后端可能还持着文件句柄，Windows 上表现为文件删不掉。
    if (player_) player_->stop();
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
