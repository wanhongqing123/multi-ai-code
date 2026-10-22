// 文件作用：选择并播放一首本地音频，关闭窗口时立即停止声音。
#include "ui/DesktopMusicPlayerDialog.h"
#if defined(MAICHAT_HAS_FFPLAY)
#include "ui/MaiFfplayVideoDialog.h"
#include "MaiFfplayAudioSession.h"
#endif

#include <QCloseEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QSlider>
#include <QStandardPaths>
#include <QTimer>
#include <QVBoxLayout>

namespace {
QPointer<DesktopMusicPlayerDialog> sActiveMusic;

QString timeLabel(qint64 microseconds) {
    const qint64 seconds = qMax<qint64>(0, microseconds / 1000000);
    return QStringLiteral("%1:%2").arg(seconds / 60).arg(seconds % 60, 2, 10, QLatin1Char('0'));
}

#if defined(MAICHAT_HAS_FFPLAY)
bool supportedMusic(const QString& path) {
    const QFileInfo file(path);
    return file.isFile() && file.isReadable() && file.size() > 0 &&
        QStringList{QStringLiteral("mp3"), QStringLiteral("m4a"), QStringLiteral("wav")}
            .contains(file.suffix().toLower());
}
#endif
}  // namespace

bool DesktopMusicPlayerDialog::openFile(QWidget* parent, const QString& path, QString* error) {
    if (error) error->clear();
#if !defined(MAICHAT_HAS_FFPLAY)
    (void)parent;
    (void)path;
    if (error) *error = QStringLiteral("当前构建未包含 FFplay 音频播放器");
    return false;
#else
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
#endif
}

void DesktopMusicPlayerDialog::closeActive() {
    if (sActiveMusic) sActiveMusic->close();
}

DesktopMusicPlayerDialog::DesktopMusicPlayerDialog(QWidget* parent, const QString& path)
    : QDialog(parent) {
    setWindowTitle(QStringLiteral("音乐播放器"));
    setFixedWidth(440);
    setObjectName(QStringLiteral("desktopMusicPlayer"));
    setStyleSheet(QStringLiteral("QDialog#desktopMusicPlayer{background:#ffffff;}"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(28, 22, 28, 22);
    layout->setSpacing(12);
    auto* source = new QLabel(QStringLiteral("正在播放  ·  本地文件"), this);
    source->setStyleSheet(QStringLiteral("color:#667085;font-size:12px;"));
    layout->addWidget(source);
    auto* artwork = new QLabel(QStringLiteral("♫"), this);
    artwork->setAlignment(Qt::AlignCenter);
    artwork->setFixedSize(104, 104);
    artwork->setStyleSheet(QStringLiteral(
        "color:#ffffff;font-size:48px;border-radius:24px;"
        "background:qlineargradient(x1:0,y1:0,x2:1,y2:1,stop:0 #2193ee,stop:1 #6350c2);"));
    layout->addWidget(artwork, 0, Qt::AlignHCenter);
    title_ = new QLabel(this);
    title_->setWordWrap(true);
    title_->setAlignment(Qt::AlignCenter);
    title_->setStyleSheet(QStringLiteral("color:#101828;font-size:17px;font-weight:600;"));
    layout->addWidget(title_);
    progress_ = new QSlider(Qt::Horizontal, this);
    progress_->setObjectName(QStringLiteral("desktopMusicProgress"));
    progress_->setRange(0, 1000);
    progress_->setStyleSheet(QStringLiteral(
        "QSlider::groove:horizontal{height:4px;background:#d6e2ee;border-radius:2px;}"
        "QSlider::sub-page:horizontal{background:#168fe0;border-radius:2px;}"
        "QSlider::handle:horizontal{background:#168fe0;width:14px;margin:-5px 0;"
        "border-radius:7px;}"));
    layout->addWidget(progress_);
    time_ = new QLabel(QStringLiteral("0:00 / 0:00"), this);
    time_->setStyleSheet(QStringLiteral("color:#667085;font-size:12px;"));
    layout->addWidget(time_);
    status_ = new QLabel(this);
    status_->setStyleSheet(QStringLiteral("color:#667085;font-size:12px;"));
    layout->addWidget(status_);
    auto* controls = new QHBoxLayout;
    play_ = new QPushButton(QStringLiteral("暂停"), this);
    play_->setObjectName(QStringLiteral("desktopMusicPlayPause"));
    auto* stop = new QPushButton(QStringLiteral("停止"), this);
    auto* choose = new QPushButton(QStringLiteral("选择其他音乐"), this);
    play_->setFixedSize(88, 36);
    stop->setFixedSize(88, 36);
    play_->setStyleSheet(QStringLiteral(
        "QPushButton{color:white;background:#168fe0;border:0;border-radius:10px;"
        "font-weight:600;}QPushButton:hover{background:#0d76bd;}"));
    stop->setStyleSheet(QStringLiteral(
        "QPushButton{color:#0d76bd;background:#e8f4fc;border:0;border-radius:10px;}"));
    choose->setStyleSheet(QStringLiteral(
        "QPushButton{color:#344054;background:transparent;border:0;padding:8px;}"));
    controls->addWidget(play_);
    controls->addWidget(stop);
    controls->addStretch();
    controls->addWidget(choose);
    layout->addLayout(controls);

    timer_ = new QTimer(this);
    timer_->setInterval(250);
    connect(timer_, &QTimer::timeout, this, &DesktopMusicPlayerDialog::refreshPlayback);
    connect(progress_, &QSlider::sliderReleased, this, [this] {
#if defined(MAICHAT_HAS_FFPLAY)
        MaiFfplayPlaybackStatus state{};
        if (session_ && maiFfplayAudioStatus(session_, &state) && state.duration_us > 0)
            maiFfplayAudioSeekAbsoluteUs(session_,
                static_cast<int64_t>(state.duration_us * (progress_->value() / 1000.0)));
#endif
    });
    connect(play_, &QPushButton::clicked, this, [this] {
#if defined(MAICHAT_HAS_FFPLAY)
        if (!session_) { setFile(path_); return; }
        int exitCode = 0;
        if (maiFfplayAudioFinished(session_, &exitCode)) { setFile(path_); return; }
        if (maiFfplayAudioCommand(session_, "pause")) {
            playing_ = !playing_;
            play_->setText(playing_ ? QStringLiteral("暂停") : QStringLiteral("播放"));
        }
#endif
    });
    connect(stop, &QPushButton::clicked, this, [this] {
        stopPlayback();
        progress_->setValue(0);
        time_->setText(QStringLiteral("0:00 / 0:00"));
        play_->setText(QStringLiteral("播放"));
    });
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
    stopPlayback();
#if defined(MAICHAT_HAS_FFPLAY)
    if (auto video = MaiFfplayVideoDialog::activeDialog()) video->close();
#endif
    path_ = path;
    title_->setText(QFileInfo(path).fileName());
    status_->clear();
    progress_->setValue(0);
    time_->setText(QStringLiteral("0:00 / 0:00"));
#if defined(MAICHAT_HAS_FFPLAY)
    const QByteArray utf8 = QFileInfo(path).absoluteFilePath().toUtf8();
    session_ = maiFfplayAudioStart(utf8.constData());
    if (!session_) {
        status_->setText(QStringLiteral("无法启动 FFplay 音频播放器"));
        play_->setText(QStringLiteral("播放"));
        return;
    }
    playing_ = true;
    play_->setText(QStringLiteral("暂停"));
    status_->setText(QStringLiteral("正在载入…"));
    timer_->start();
#else
    status_->setText(QStringLiteral("当前构建未包含 FFplay 音频播放器"));
#endif
}

void DesktopMusicPlayerDialog::refreshPlayback() {
#if defined(MAICHAT_HAS_FFPLAY)
    if (!session_) return;
    MaiFfplayPlaybackStatus state{};
    if (maiFfplayAudioStatus(session_, &state) && state.duration_us > 0) {
        status_->clear();
        const auto position = qBound<int64_t>(0, state.position_us, state.duration_us);
        if (!progress_->isSliderDown())
            progress_->setValue(static_cast<int>(position * 1000.0 / state.duration_us));
        time_->setText(timeLabel(position) + QStringLiteral(" / ") +
                       timeLabel(state.duration_us));
        playing_ = state.paused == 0;
        play_->setText(playing_ ? QStringLiteral("暂停") : QStringLiteral("播放"));
        return;
    }
    int exitCode = 0;
    if (maiFfplayAudioFinished(session_, &exitCode)) {
        timer_->stop();
        playing_ = false;
        play_->setText(QStringLiteral("重播"));
        if (exitCode != 0) status_->setText(QStringLiteral("此文件无法播放"));
    }
#endif
}

void DesktopMusicPlayerDialog::stopPlayback() {
    timer_->stop();
#if defined(MAICHAT_HAS_FFPLAY)
    if (session_) {
        maiFfplayAudioStop(session_);
        session_ = nullptr;
    }
#endif
    playing_ = false;
}

void DesktopMusicPlayerDialog::closeEvent(QCloseEvent* event) {
    stopPlayback();
    if (sActiveMusic == this) sActiveMusic.clear();
    QDialog::closeEvent(event);
}

DesktopMusicPlayerDialog::~DesktopMusicPlayerDialog() { stopPlayback(); }
