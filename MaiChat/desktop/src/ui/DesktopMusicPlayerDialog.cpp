// 文件作用：选择并播放一首本地音频，关闭窗口时立即停止声音。
#include "ui/DesktopMusicPlayerDialog.h"

#include <climits>

#include <QCloseEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMediaPlayer>
#include <QPointer>
#include <QPushButton>
#include <QSlider>
#include <QStandardPaths>
#include <QUrl>
#include <QVBoxLayout>

namespace {
QPointer<DesktopMusicPlayerDialog> sActiveMusic;

QString timeLabel(qint64 milliseconds) {
    const qint64 seconds = qMax<qint64>(0, milliseconds / 1000);
    return QStringLiteral("%1:%2").arg(seconds / 60).arg(seconds % 60, 2, 10, QLatin1Char('0'));
}

bool supportedMusic(const QString& path) {
    const QFileInfo file(path);
    return file.isFile() && file.isReadable() && file.size() > 0 &&
        QStringList{QStringLiteral("mp3"), QStringLiteral("m4a"), QStringLiteral("wav")}
            .contains(file.suffix().toLower());
}
}  // namespace

bool DesktopMusicPlayerDialog::openFile(QWidget* parent, const QString& path, QString* error) {
    if (error) error->clear();
    if (!supportedMusic(path)) {
        if (error) *error = QStringLiteral("请选择可读取的 MP3、M4A 或 WAV 文件");
        return false;
    }
    if (sActiveMusic) sActiveMusic->close();
    auto* dialog = new DesktopMusicPlayerDialog(parent, path);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    sActiveMusic = dialog;
    dialog->show();
    dialog->raise();
    return true;
}

DesktopMusicPlayerDialog::DesktopMusicPlayerDialog(QWidget* parent, const QString& path)
    : QDialog(parent) {
    setWindowTitle(QStringLiteral("本地音乐"));
    setMinimumWidth(420);
    setObjectName(QStringLiteral("desktopMusicPlayer"));
    auto* layout = new QVBoxLayout(this);
    title_ = new QLabel(this);
    title_->setWordWrap(true);
    layout->addWidget(title_);
    progress_ = new QSlider(Qt::Horizontal, this);
    progress_->setObjectName(QStringLiteral("desktopMusicProgress"));
    progress_->setRange(0, 0);
    layout->addWidget(progress_);
    time_ = new QLabel(QStringLiteral("0:00 / 0:00"), this);
    layout->addWidget(time_);
    status_ = new QLabel(this);
    layout->addWidget(status_);
    auto* controls = new QHBoxLayout;
    play_ = new QPushButton(QStringLiteral("暂停"), this);
    play_->setObjectName(QStringLiteral("desktopMusicPlayPause"));
    auto* stop = new QPushButton(QStringLiteral("停止"), this);
    auto* choose = new QPushButton(QStringLiteral("选择其他音乐"), this);
    controls->addWidget(play_);
    controls->addWidget(stop);
    controls->addStretch();
    controls->addWidget(choose);
    layout->addLayout(controls);

    player_ = new QMediaPlayer(this);
    connect(player_, &QMediaPlayer::durationChanged, this, [this](qint64 duration) {
        progress_->setMaximum(static_cast<int>(qMin<qint64>(duration, INT_MAX)));
        time_->setText(timeLabel(player_->position()) + QStringLiteral(" / ") + timeLabel(duration));
    });
    connect(player_, &QMediaPlayer::positionChanged, this, [this](qint64 position) {
        if (!progress_->isSliderDown())
            progress_->setValue(static_cast<int>(qMin<qint64>(position, INT_MAX)));
        time_->setText(timeLabel(position) + QStringLiteral(" / ") + timeLabel(player_->duration()));
    });
    connect(player_, &QMediaPlayer::stateChanged, this, [this](QMediaPlayer::State state) {
        play_->setText(state == QMediaPlayer::PlayingState ? QStringLiteral("暂停") :
            QStringLiteral("播放"));
    });
    connect(player_, QOverload<QMediaPlayer::Error>::of(&QMediaPlayer::error), this,
            [this](QMediaPlayer::Error) {
                status_->setText(player_->errorString().isEmpty() ?
                    QStringLiteral("此文件无法播放") : player_->errorString());
            });
    connect(progress_, &QSlider::sliderReleased, this,
            [this] { player_->setPosition(progress_->value()); });
    connect(play_, &QPushButton::clicked, this, [this] {
        if (player_->state() == QMediaPlayer::PlayingState) player_->pause();
        else player_->play();
    });
    connect(stop, &QPushButton::clicked, player_, &QMediaPlayer::stop);
    connect(choose, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("选择本地音乐"),
            QStandardPaths::writableLocation(QStandardPaths::MusicLocation),
            QStringLiteral("音频文件 (*.mp3 *.m4a *.wav)"));
        if (path.isEmpty()) return;
        if (!supportedMusic(path)) {
            status_->setText(QStringLiteral("请选择可读取的 MP3、M4A 或 WAV 文件"));
            return;
        }
        setFile(path);
    });
    setFile(path);
}

void DesktopMusicPlayerDialog::setFile(const QString& path) {
    player_->stop();
    title_->setText(QFileInfo(path).fileName());
    status_->clear();
    progress_->setValue(0);
    player_->setMedia(QUrl::fromLocalFile(path));
    player_->play();
}

void DesktopMusicPlayerDialog::closeEvent(QCloseEvent* event) {
    player_->stop();
    if (sActiveMusic == this) sActiveMusic.clear();
    QDialog::closeEvent(event);
}
