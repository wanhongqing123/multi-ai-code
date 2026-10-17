// 文件作用：桌面端共用界面；MaiFfplayVideoDialog 负责对话框呈现、确认交互和结果回传，本文件给出其实现。
// 主要入口：setForwardAction、forwardVideo、saveCopy、activeDialog。
// 直接依赖：MaiGraphicsPresenter.h；实现与这些接口协作。
#include "ui/MaiFfplayVideoDialog.h"

#include <QCloseEvent>
#include <QComboBox>
#include <QApplication>
#include <QClipboard>
#include <QCoreApplication>
#include <QDebug>
#include <QDesktopServices>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMetaObject>
#include <QMessageBox>
#include <QMimeData>
#include <QMenu>
#include <QPushButton>
#include <QResizeEvent>
#include <QSaveFile>
#include <QSignalBlocker>
#include <QShowEvent>
#include <QSlider>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <atomic>
#include <cmath>

#include "MaiGraphicsPresenter.h"

#if defined(Q_OS_MAC)
extern "C" void* maiGraphicsCreateMacLayer(QWidget* widget);
extern "C" void maiGraphicsResizeMacLayer(void* nativeLayer, QWidget* widget);
extern "C" void maiGraphicsRetainMacLayer(void* nativeLayer);
extern "C" void maiGraphicsReleaseMacLayer(void* nativeLayer);
extern "C" void maiGraphicsStyleMacVideoWindow(QWidget* widget);
extern "C" void maiGraphicsShareMacVideo(QWidget* anchor, const char* utf8Path);
#endif

namespace {

#if defined(Q_OS_MAC)
class MaiVideoSurface final : public QWidget {
public:
    explicit MaiVideoSurface(QWidget* parent) : QWidget(parent) {}
    QPaintEngine* paintEngine() const override { return nullptr; }
};
#endif

QPointer<MaiFfplayVideoDialog> sActiveDialog;
int sPresenterUsers = 0;
std::atomic<uint64_t> sPresentedFrames{0};

void onFramePresented(uint64_t, bool success, void*) {
    if (success) sPresentedFrames.fetch_add(1);
}

QString bundledResource(const QString& relative) {
    const QString base = QCoreApplication::applicationDirPath();
#if defined(Q_OS_WIN)
    if (relative.endsWith(QStringLiteral("libmaiagent_obs_metal.so")))
        return base + QStringLiteral("/maiagent_obs_d3d11.dll");
    return base + QStringLiteral("/MaiAgentGraphics");
#else
    const QString bundled = QFileInfo(base + relative).absoluteFilePath();
    if (QFileInfo::exists(bundled)) return bundled;
    const QString development = QDir(base).filePath(relative.endsWith(
        QStringLiteral("libmaiagent_obs_metal.so")) ?
        QStringLiteral("../maiagent/Graphics/libmaiagent_obs_metal.so") :
        QStringLiteral("../maiagent/Graphics/data"));
    return QFileInfo(development).absoluteFilePath();
#endif
}

}  // namespace

// 构造时一次性连接播放器、进度条和窗口关闭行为；关闭需停止解码与音频后再销毁界面。
MaiFfplayVideoDialog::MaiFfplayVideoDialog(const QString& path, QWidget* parent)
    : QDialog(parent), path_(path) {
    setWindowTitle(QFileInfo(path).fileName());
    setMinimumSize(640, 420);
#if defined(Q_OS_MAC)
    setStyleSheet(QStringLiteral("QDialog { background: #080c14; }"));
#else
    setStyleSheet(QStringLiteral("QDialog { background: #10151f; }"));
#endif
    auto* layout = new QVBoxLayout(this);
#if defined(Q_OS_MAC)
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
#endif
#if defined(Q_OS_MAC)
    surface_ = new MaiVideoSurface(this);
#else
    surface_ = new QWidget(this);
#endif
    surface_->setObjectName(QStringLiteral("ffplayVideoSurface"));
    surface_->setAttribute(Qt::WA_NativeWindow);
#if defined(Q_OS_WIN) || defined(Q_OS_MAC)
    surface_->setAttribute(Qt::WA_PaintOnScreen);
    surface_->setAttribute(Qt::WA_NoSystemBackground);
#else
    surface_->setStyleSheet(QStringLiteral("background: black"));
#endif
    layout->addWidget(surface_, 1);
    QWidget* controlParent = this;
    QVBoxLayout* panelLayout = nullptr;
#if defined(Q_OS_MAC)
    overlay_ = new QWidget(surface_);
    overlay_->setObjectName(QStringLiteral("ffplayOverlay"));
    overlay_->setAttribute(Qt::WA_NativeWindow);
    overlay_->setAttribute(Qt::WA_TranslucentBackground);
    overlay_->setStyleSheet(QStringLiteral("QWidget#ffplayOverlay { background: transparent; }"));
    overlay_->setGeometry(surface_->rect());
    surface_->installEventFilter(this);
    auto* overlayLayout = new QVBoxLayout(overlay_);
    overlayLayout->setContentsMargins(18, 18, 18, 18);
    errorLabel_ = new QLabel(overlay_);
    errorLabel_->setAlignment(Qt::AlignCenter);
    overlayLayout->addStretch(1);
    overlayLayout->addWidget(errorLabel_);
    auto* panel = new QWidget(overlay_);
    panel->setObjectName(QStringLiteral("ffplayControlPanel"));
    panel->setStyleSheet(QStringLiteral(
        "QWidget#ffplayControlPanel { background: rgba(9, 15, 25, 218);"
        " border: 1px solid rgba(255,255,255,45); border-radius: 16px; }"));
    panelLayout = new QVBoxLayout(panel);
    panelLayout->setContentsMargins(16, 12, 16, 10);
    panelLayout->setSpacing(8);
    overlayLayout->addWidget(panel);
    controlParent = panel;
#else
    errorLabel_ = new QLabel(this);
    layout->addWidget(errorLabel_);
#endif
    errorLabel_->setStyleSheet(QStringLiteral("color: #ffb4b4"));
    errorLabel_->hide();
    auto* controls = new QHBoxLayout();
#if defined(Q_OS_MAC)
    pauseButton_ = new QPushButton(QStringLiteral("⏸"), controlParent);
#else
    pauseButton_ = new QPushButton(QStringLiteral("Ⅱ"), controlParent);
#endif
    pauseButton_->setObjectName(QStringLiteral("ffplayPauseButton"));
    pauseButton_->setToolTip(QStringLiteral("暂停视频"));
    seekSlider_ = new QSlider(Qt::Horizontal, controlParent);
    seekSlider_->setObjectName(QStringLiteral("ffplaySeekSlider"));
    seekSlider_->setRange(0, 1000);
    timeLabel_ = new QLabel(QStringLiteral("0:00 / 0:00"), controlParent);
    timeLabel_->setObjectName(QStringLiteral("ffplayTimeLabel"));
    timeLabel_->setStyleSheet(QStringLiteral("color: white"));
    controls->addWidget(pauseButton_);
    controls->addWidget(seekSlider_, 1);
    controls->addWidget(timeLabel_);
    speedSelector_ = new QComboBox(controlParent);
    speedSelector_->setObjectName(QStringLiteral("ffplaySpeedSelector"));
    speedSelector_->setToolTip(QStringLiteral("播放倍速"));
    for (double rate : {0.25, 0.5, 1.0, 1.5, 2.0})
        speedSelector_->addItem(QStringLiteral("%1×").arg(rate), rate);
    speedSelector_->setCurrentIndex(2);
    controls->addWidget(speedSelector_);
#if defined(Q_OS_MAC)
    panelLayout->addLayout(controls);
    pauseButton_->setFixedSize(38, 38);
    pauseButton_->setStyleSheet(QStringLiteral(
        "QPushButton#ffplayPauseButton { color: white; background: rgba(255,255,255,30);"
        " border: none; border-radius: 19px; font-size: 19px; }"
        "QPushButton#ffplayPauseButton:hover { background: rgba(255,255,255,65); }"));
    seekSlider_->setStyleSheet(QStringLiteral(
        "QSlider::groove:horizontal { height: 4px; background: rgba(255,255,255,75);"
        " border-radius: 2px; }"
        "QSlider::sub-page:horizontal { background: #f5f8ff; border-radius: 2px; }"
        "QSlider::handle:horizontal { width: 14px; margin: -5px 0;"
        " background: white; border: none; border-radius: 7px; }"));
    timeLabel_->setStyleSheet(QStringLiteral("color: white; font-size: 12px;"));
    QFont clockFont = timeLabel_->font();
    clockFont.setStyleHint(QFont::Monospace);
    timeLabel_->setFont(clockFont);
    speedSelector_->setStyleSheet(QStringLiteral(
        "QComboBox { color: white; background: rgba(255,255,255,24); border: none;"
        " border-radius: 10px; padding: 6px 12px; min-width: 42px; }"
        "QComboBox::drop-down { border: none; width: 0; }"
        "QComboBox QAbstractItemView { color: white; background: #182132;"
        " border: 1px solid #536173; selection-background-color: #35557a; }"));
#else
    layout->addLayout(controls);
#endif
    progressTimer_ = new QTimer(this);
    progressTimer_->setInterval(250);
    connect(progressTimer_, &QTimer::timeout, this, [this] {
        MaiFfplayPlaybackStatus status{};
        if (!maiFfplayGetPlaybackStatus(&status) || status.duration_us <= 0) return;
        const auto position = std::clamp(status.position_us, int64_t{0}, status.duration_us);
        if (!seekSlider_->isSliderDown())
            seekSlider_->setValue(static_cast<int>(position * 1000.0 / status.duration_us));
        const auto clockText = [](int64_t microseconds) {
            const int64_t seconds = microseconds / 1000000;
            return QStringLiteral("%1:%2").arg(static_cast<qlonglong>(seconds / 60))
                .arg(static_cast<qlonglong>(seconds % 60), 2, 10, QLatin1Char('0'));
        };
        timeLabel_->setText(clockText(position) + QStringLiteral(" / ") +
                            clockText(status.duration_us));
        playing_ = status.paused == 0;
#if defined(Q_OS_MAC)
        pauseButton_->setText(playing_ ? QStringLiteral("⏸") : QStringLiteral("▶"));
#else
        pauseButton_->setText(playing_ ? QStringLiteral("Ⅱ") : QStringLiteral("▶"));
#endif
        pauseButton_->setToolTip(playing_ ? QStringLiteral("暂停视频") : QStringLiteral("播放视频"));
    });
    connect(pauseButton_, &QPushButton::clicked, this, [this] {
        sendCommand(QStringLiteral("pause"));
    });
    connect(seekSlider_, &QSlider::sliderReleased, this, [this] {
        seekPercent(seekSlider_->value() / 1000.0);
    });
    connect(speedSelector_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int index) {
                if (index >= 0) setSpeed(speedSelector_->itemData(index).toDouble());
            });
    auto* actions = new QHBoxLayout();
    actions->addStretch(1);
#if defined(Q_OS_MAC)
    auto* forward = new QPushButton(QStringLiteral("↗  转发"), controlParent);
#else
    auto* forward = new QPushButton(QStringLiteral("↗"), controlParent);
#endif
    forward->setObjectName(QStringLiteral("videoForwardButton"));
#if defined(Q_OS_MAC)
    forward->setStyleSheet(QStringLiteral(
        "QPushButton { color:white;background:rgba(255,255,255,25);border:0;border-radius:10px;"
        "padding:6px 12px; }"
        "QPushButton:hover { background: rgba(255,255,255,55); }"));
#else
    forward->setStyleSheet(QStringLiteral(
        "color:white;background:#363636;border:0;border-radius:20px;"
        "min-width:40px;min-height:40px;"));
#endif
    forward->setToolTip(QStringLiteral("转发视频"));
    connect(forward, &QPushButton::clicked, this, &MaiFfplayVideoDialog::forwardVideo);
    actions->addWidget(forward);
#if defined(Q_OS_MAC)
    auto* save = new QPushButton(QStringLiteral("↓  保存"), controlParent);
#else
    auto* save = new QPushButton(QStringLiteral("↓"), controlParent);
#endif
    save->setObjectName(QStringLiteral("videoSaveButton"));
    save->setStyleSheet(forward->styleSheet());
    save->setToolTip(QStringLiteral("保存视频副本"));
    connect(save, &QPushButton::clicked, this, &MaiFfplayVideoDialog::saveCopy);
    actions->addWidget(save);
    auto* more = new QPushButton(QStringLiteral("⋯"), controlParent);
    more->setToolTip(QStringLiteral("更多操作"));
    more->setStyleSheet(forward->styleSheet());
    connect(more, &QPushButton::clicked, this, [this, more] {
        QMenu menu(this);
#if defined(Q_OS_MAC)
        QAction* systemShare = menu.addAction(QStringLiteral("使用 macOS 分享…"));
#endif
        QAction* copyPath = menu.addAction(QStringLiteral("复制文件路径"));
        QAction* openFolder = menu.addAction(QStringLiteral("打开所在文件夹"));
        QAction* chosen = menu.exec(more->mapToGlobal(more->rect().bottomLeft()));
#if defined(Q_OS_MAC)
        if (chosen == systemShare) {
            const QByteArray utf8 = path_.toUtf8();
            maiGraphicsShareMacVideo(more, utf8.constData());
        } else
#endif
        if (chosen == copyPath) QApplication::clipboard()->setText(path_);
        else if (chosen == openFolder)
            QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path_).absolutePath()));
    });
    actions->addWidget(more);
#if defined(Q_OS_MAC)
    previousButton_ = new QPushButton(QStringLiteral("‹"), controlParent);
    nextButton_ = new QPushButton(QStringLiteral("›"), controlParent);
    previousButton_->setObjectName(QStringLiteral("ffplayPreviousItemButton"));
    nextButton_->setObjectName(QStringLiteral("ffplayNextItemButton"));
    for (QPushButton* button : {previousButton_, nextButton_}) {
        button->setFixedSize(34, 32);
        button->setStyleSheet(QStringLiteral(
            "color:white;background:rgba(255,255,255,25);border:0;border-radius:10px;"
            "font-size:22px;"));
        button->hide();
    }
    previousButton_->setToolTip(QStringLiteral("上一段视频"));
    nextButton_->setToolTip(QStringLiteral("下一段视频"));
    connect(previousButton_, &QPushButton::clicked, this, [this] { navigateVideo(-1); });
    connect(nextButton_, &QPushButton::clicked, this, [this] { navigateVideo(1); });
    actions->insertWidget(0, previousButton_);
    actions->insertWidget(1, nextButton_);
    auto* replayButton = new QPushButton(QStringLiteral("↺  重播"), controlParent);
    replayButton->setObjectName(QStringLiteral("ffplayReplayButton"));
    replayButton->setStyleSheet(forward->styleSheet());
    replayButton->setToolTip(QStringLiteral("从头播放"));
    connect(replayButton, &QPushButton::clicked, this, [this] { replay(); });
    actions->insertWidget(3, replayButton);
    auto* copyPath = new QPushButton(QStringLiteral("⧉  复制路径"), controlParent);
    copyPath->setObjectName(QStringLiteral("ffplayCopyPathButton"));
    copyPath->setStyleSheet(forward->styleSheet());
    connect(copyPath, &QPushButton::clicked, this, [this] {
        QApplication::clipboard()->setText(path_);
    });
    actions->insertWidget(actions->count() - 1, copyPath);
    auto* fullscreenButton = new QPushButton(QStringLiteral("⛶"), controlParent);
    fullscreenButton->setObjectName(QStringLiteral("ffplayFullscreenButton"));
    fullscreenButton->setToolTip(QStringLiteral("切换全屏"));
    fullscreenButton->setStyleSheet(forward->styleSheet());
    connect(fullscreenButton, &QPushButton::clicked, this, [this] {
        if (isFullScreen()) showNormal();
        else showFullScreen();
    });
    actions->insertWidget(actions->count() - 1, fullscreenButton);
    panelLayout->addLayout(actions);
    overlay_->raise();
#else
    layout->addLayout(actions);
#endif
}

void MaiFfplayVideoDialog::setForwardAction(std::function<void(const QString&)> action) {
    forwardAction_ = std::move(action);
}

void MaiFfplayVideoDialog::forwardVideo() {
    if (forwardAction_) {
        forwardAction_(path_);
        return;
    }
    auto* media = new QMimeData();
    media->setUrls({QUrl::fromLocalFile(path_)});
    QApplication::clipboard()->setMimeData(media);
    QMessageBox::information(this, QStringLiteral("视频已复制"),
                             QStringLiteral("可在支持粘贴文件的应用中转发。"));
}

void MaiFfplayVideoDialog::saveCopy() {
    const QString target = QFileDialog::getSaveFileName(
        this, QStringLiteral("保存视频"), QFileInfo(path_).fileName(),
        QStringLiteral("视频文件 (*.mp4 *.mov *.m4v);;所有文件 (*)"));
    if (target.isEmpty()) return;
    if (QFileInfo(target).absoluteFilePath() == QFileInfo(path_).absoluteFilePath()) return;
    QFile source(path_);
    QSaveFile output(target);
    if (!source.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly)) {
        QMessageBox::warning(this, QStringLiteral("保存失败"), QStringLiteral("无法读取或写入视频文件。"));
        return;
    }
    while (!source.atEnd()) {
        const QByteArray bytes = source.read(1024 * 1024);
        if (bytes.isEmpty() || output.write(bytes) != bytes.size()) {
            output.cancelWriting();
            QMessageBox::warning(this, QStringLiteral("保存失败"), QStringLiteral("复制视频时发生错误。"));
            return;
        }
    }
    if (!output.commit()) {
        QMessageBox::warning(this, QStringLiteral("保存失败"), QStringLiteral("无法完成视频保存。"));
        return;
    }
    QMessageBox::information(this, QStringLiteral("已保存"), QStringLiteral("视频副本已保存。"));
}

MaiFfplayVideoDialog::~MaiFfplayVideoDialog() { stopPlayback(); }

QPointer<MaiFfplayVideoDialog> MaiFfplayVideoDialog::activeDialog() {
    return sActiveDialog;
}

uint64_t MaiFfplayVideoDialog::presentedFrames() const {
    return sPresentedFrames.load();
}

bool MaiFfplayVideoDialog::presentVideo(void* userData, const MaiVideoFrame* frame,
                                        const MaiVideoSubtitle* subtitle) {
    const auto* dialog = static_cast<MaiFfplayVideoDialog*>(userData);
    return maiGraphicsPresenterShowVideoFrameWithSubtitle(dialog->viewId_, frame,
                                                          subtitle, false);
}

bool MaiFfplayVideoDialog::presentRgba(void* userData, const uint8_t* pixels,
                                       uint32_t width, uint32_t height, uint32_t stride) {
    const auto* dialog = static_cast<MaiFfplayVideoDialog*>(userData);
    return maiGraphicsPresenterShowFrame(dialog->viewId_, pixels, width, height,
                                         stride, false);
}

void MaiFfplayVideoDialog::setTitle(void* userData, const char* title) {
    auto* dialog = static_cast<MaiFfplayVideoDialog*>(userData);
#if defined(Q_OS_MAC)
    const QString value = QFileInfo(dialog->path_).fileName();
#else
    const QString value = QString::fromUtf8(title ? title : "Video");
#endif
    QMetaObject::invokeMethod(dialog, [dialog, value] { dialog->setWindowTitle(value); },
                              Qt::QueuedConnection);
}

void MaiFfplayVideoDialog::setSize(void* userData, int width, int height) {
    auto* dialog = static_cast<MaiFfplayVideoDialog*>(userData);
    QMetaObject::invokeMethod(dialog, [dialog, width, height] {
#if defined(Q_OS_MAC)
        dialog->resize(std::max(width, 640), std::max(height, 420));
#else
        dialog->resize(std::max(width, 640), std::max(height + 60, 420));
#endif
    }, Qt::QueuedConnection);
}

void MaiFfplayVideoDialog::setPosition(void* userData, int x, int y) {
    auto* dialog = static_cast<MaiFfplayVideoDialog*>(userData);
    QMetaObject::invokeMethod(dialog, [dialog, x, y] { dialog->move(x, y); },
                              Qt::QueuedConnection);
}

void MaiFfplayVideoDialog::setFullscreen(void* userData, int enabled) {
    auto* dialog = static_cast<MaiFfplayVideoDialog*>(userData);
    QMetaObject::invokeMethod(dialog, [dialog, enabled] {
        if (enabled) dialog->showFullScreen();
        else dialog->showNormal();
    }, Qt::QueuedConnection);
}

void MaiFfplayVideoDialog::showWindow(void* userData) {
    auto* dialog = static_cast<MaiFfplayVideoDialog*>(userData);
    QMetaObject::invokeMethod(dialog, [dialog] { dialog->raise(); },
                              Qt::QueuedConnection);
}

// 作用：验证文件并启动 FFplay，同时同步播放控件状态。
bool MaiFfplayVideoDialog::startPlayback() {
    if (started_ || !QFileInfo(path_).isFile()) {
        qWarning() << "FFplay popup has no readable video file:" << path_;
        return false;
    }
    errorLabel_->hide();
    if (!sActiveDialog.isNull() && sActiveDialog != this) sActiveDialog->close();
    const QString backend = bundledResource(
        QStringLiteral("/../Frameworks/libmaiagent_obs_metal.so"));
    const QString effects = bundledResource(
        QStringLiteral("/../Resources/MaiAgentGraphics"));
    if (!QFileInfo::exists(backend) ||
        !QFileInfo::exists(effects + QStringLiteral("/default.effect"))) {
        qWarning() << "FFplay popup is missing Graphics resources:" << backend << effects;
        return false;
    }
    if (sPresenterUsers == 0) {
        const QByteArray modulePath = backend.toUtf8();
        const QByteArray effectPath = effects.toUtf8();
        if (!maiGraphicsPresenterStart(modulePath.constData(), effectPath.constData(),
                                       onFramePresented, nullptr)) {
            qWarning() << "FFplay popup could not start Graphics:" << backend << effects;
            return false;
        }
    }
    ++sPresenterUsers;
#if defined(Q_OS_WIN)
    nativeView_ = reinterpret_cast<void*>(surface_->winId());
    if (nativeView_) {
        viewId_ = maiGraphicsPresenterAttach(nativeView_,
                                             std::max(surface_->width(), 1),
                                             std::max(surface_->height(), 1), nullptr, nullptr);
    }
#else
    nativeView_ = maiGraphicsCreateMacLayer(surface_);
    if (nativeView_) {
        viewId_ = maiGraphicsPresenterAttach(nativeView_,
                                             std::max(surface_->width(), 1),
                                             std::max(surface_->height(), 1),
                                             maiGraphicsRetainMacLayer,
                                             maiGraphicsReleaseMacLayer);
        maiGraphicsReleaseMacLayer(nativeView_);
    }
#endif
    if (!viewId_) {
        qWarning() << "FFplay popup could not attach its video surface:" << path_;
        --sPresenterUsers;
        if (sPresenterUsers == 0) maiGraphicsPresenterStop();
        return false;
    }
#if defined(Q_OS_MAC)
    if (overlay_) overlay_->raise();
#endif
    host_.user_data = this;
    host_.graphics_view_id = viewId_;
    host_.present_video = presentVideo;
    host_.present_rgba = presentRgba;
    host_.set_title = setTitle;
    host_.set_size = setSize;
    host_.set_position = setPosition;
    host_.set_fullscreen = setFullscreen;
    host_.show_window = showWindow;
    const QByteArray sourcePath = QFileInfo(path_).absoluteFilePath().toUtf8();
    sPresentedFrames = 0;
    started_ = true;
    progressTimer_->start();
    playbackFinished_ = false;
    sActiveDialog = this;
    const double speed = playbackSpeed_;
    const int64_t startPositionUs = startPositionUs_;
    playbackThread_ = std::thread([this, sourcePath, speed, startPositionUs] {
        char name[] = "ffplay";
        char* arguments[] = {name, const_cast<char*>(sourcePath.constData())};
        const int result = maiFfplayRunAtSpeed(&host_, 2, arguments, speed, startPositionUs);
        playbackFinished_ = true;
        QMetaObject::invokeMethod(this, [this, result] {
            if (result != 0) {
                errorLabel_->setText(QStringLiteral("无法播放该视频（FFplay 状态 %1）").arg(result));
                errorLabel_->show();
            }
        }, Qt::QueuedConnection);
    });
    return true;
}

void MaiFfplayVideoDialog::stopPlayback() {
    if (!started_) return;
    progressTimer_->stop();
    for (int attempt = 0; attempt < 100 && !playbackFinished_; ++attempt) {
        if (maiFfplaySendCommand("close") == 1) break;
        QThread::msleep(10);
    }
    if (playbackThread_.joinable()) playbackThread_.join();
    if (viewId_) maiGraphicsPresenterDetach(viewId_);
    viewId_ = 0;
    if (--sPresenterUsers == 0) maiGraphicsPresenterStop();
    if (sActiveDialog == this) sActiveDialog = nullptr;
    started_ = false;
}

bool MaiFfplayVideoDialog::sendCommand(const QString& command) {
    const QByteArray utf8 = command.toUtf8();
    const bool accepted = maiFfplaySendCommand(utf8.constData()) == 1;
    if (accepted && command == QStringLiteral("pause")) {
        playing_ = !playing_;
#if defined(Q_OS_MAC)
        pauseButton_->setText(playing_ ? QStringLiteral("⏸") : QStringLiteral("▶"));
#else
        pauseButton_->setText(playing_ ? QStringLiteral("暂停") : QStringLiteral("播放"));
#endif
    }
    return accepted;
}

bool MaiFfplayVideoDialog::seekPercent(double fraction) {
    return maiFfplaySeekPercent(fraction) == 1;
}

bool MaiFfplayVideoDialog::seekSeconds(double seconds) {
    MaiFfplayPlaybackStatus status{};
    if (!std::isfinite(seconds) || seconds < 0 || !maiFfplayGetPlaybackStatus(&status) ||
        status.duration_us <= 0 || seconds * 1'000'000 > status.duration_us)
        return false;
    return seekPercent(seconds * 1'000'000 / status.duration_us);
}

bool MaiFfplayVideoDialog::setSpeed(double speed) {
    if (speed != 0.25 && speed != 0.5 && speed != 1.0 && speed != 1.5 && speed != 2.0)
        return false;
    if (!started_) return false;
    if (speed == playbackSpeed_) return true;
    MaiFfplayPlaybackStatus status{};
    if (!maiFfplayGetPlaybackStatus(&status)) return false;
    const bool wasPlaying = playing_;
    const int64_t sourcePositionUs = std::max<int64_t>(0, status.position_us);
    stopPlayback();
    playbackSpeed_ = speed;
    startPositionUs_ = sourcePositionUs;
    playing_ = true;
    if (!startPlayback()) return false;
    if (speedSelector_) {
        const QSignalBlocker blocked(speedSelector_);
        for (int index = 0; index < speedSelector_->count(); ++index) {
            if (speedSelector_->itemData(index).toDouble() == speed) {
                speedSelector_->setCurrentIndex(index);
                break;
            }
        }
    }
    if (!wasPlaying)
        QTimer::singleShot(150, this, [this] {
            if (started_ && playing_) sendCommand(QStringLiteral("pause"));
        });
    return true;
}

bool MaiFfplayVideoDialog::replay() {
    if (!started_) return false;
    stopPlayback();
    startPositionUs_ = 0;
    playing_ = true;
    return startPlayback();
}

void MaiFfplayVideoDialog::setPlaylist(const QStringList& paths) {
    playlist_.clear();
    for (const QString& path : paths) {
        if (QFileInfo(path).isFile() && !playlist_.contains(path)) playlist_.append(path);
    }
    if (!playlist_.contains(path_)) playlist_.prepend(path_);
    playlistIndex_ = playlist_.indexOf(path_);
    const bool hasAdjacent = playlist_.size() > 1;
    if (previousButton_) previousButton_->setVisible(hasAdjacent);
    if (nextButton_) nextButton_->setVisible(hasAdjacent);
    if (previousButton_) previousButton_->setEnabled(playlistIndex_ > 0);
    if (nextButton_) nextButton_->setEnabled(playlistIndex_ + 1 < playlist_.size());
}

bool MaiFfplayVideoDialog::navigateVideo(int direction) {
    if (direction != -1 && direction != 1) return false;
    const int nextIndex = playlistIndex_ + direction;
    if (nextIndex < 0 || nextIndex >= playlist_.size()) return false;
    const QString nextPath = playlist_.at(nextIndex);
    if (!QFileInfo(nextPath).isFile()) {
        errorLabel_->setText(QStringLiteral("视频文件已被移动或删除。"));
        errorLabel_->show();
        return false;
    }
    const QString previousPath = path_;
    const int previousIndex = playlistIndex_;
    stopPlayback();
    path_ = nextPath;
    playlistIndex_ = nextIndex;
    startPositionUs_ = 0;
    playing_ = true;
    setWindowTitle(QFileInfo(path_).fileName());
    if (!startPlayback()) {
        path_ = previousPath;
        playlistIndex_ = previousIndex;
        startPlayback();
        errorLabel_->setText(QStringLiteral("无法打开下一段视频。"));
        errorLabel_->show();
        return false;
    }
    if (previousButton_) previousButton_->setEnabled(playlistIndex_ > 0);
    if (nextButton_) nextButton_->setEnabled(playlistIndex_ + 1 < playlist_.size());
    return true;
}

void MaiFfplayVideoDialog::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
#if defined(Q_OS_MAC)
    maiGraphicsStyleMacVideoWindow(this);
#endif
    if (!started_ && !startPlayback()) setWindowTitle(QStringLiteral("无法打开视频"));
}

void MaiFfplayVideoDialog::closeEvent(QCloseEvent* event) {
    stopPlayback();
    QDialog::closeEvent(event);
}

void MaiFfplayVideoDialog::resizeEvent(QResizeEvent* event) {
    QDialog::resizeEvent(event);
    if (!viewId_) return;
#if defined(Q_OS_MAC)
    maiGraphicsResizeMacLayer(nativeView_, surface_);
#endif
    maiGraphicsPresenterResize(viewId_, std::max(surface_->width(), 1),
                               std::max(surface_->height(), 1));
}

bool MaiFfplayVideoDialog::eventFilter(QObject* watched, QEvent* event) {
#if defined(Q_OS_MAC)
    if (watched == surface_ && event->type() == QEvent::Resize && overlay_)
        overlay_->setGeometry(surface_->rect());
#endif
    return QDialog::eventFilter(watched, event);
}

void MaiFfplayVideoDialog::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) close();
    else if (event->key() == Qt::Key_Space) sendCommand(QStringLiteral("pause"));
    else if (event->key() == Qt::Key_Left && event->modifiers() & Qt::ShiftModifier) navigateVideo(-1);
    else if (event->key() == Qt::Key_Right && event->modifiers() & Qt::ShiftModifier) navigateVideo(1);
    else if (event->key() == Qt::Key_Left) sendCommand(QStringLiteral("seek_backward"));
    else if (event->key() == Qt::Key_Right) sendCommand(QStringLiteral("seek_forward"));
    else if (event->key() == Qt::Key_PageUp) sendCommand(QStringLiteral("next_chapter"));
    else if (event->key() == Qt::Key_PageDown) sendCommand(QStringLiteral("previous_chapter"));
    else QDialog::keyPressEvent(event);
}
