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
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
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
#endif

namespace {

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
    setStyleSheet(QStringLiteral("QDialog { background: #10151f; }"));
    auto* layout = new QVBoxLayout(this);
    surface_ = new QWidget(this);
    surface_->setObjectName(QStringLiteral("ffplayVideoSurface"));
    surface_->setAttribute(Qt::WA_NativeWindow);
#if defined(Q_OS_WIN) || defined(Q_OS_MAC)
    surface_->setAttribute(Qt::WA_PaintOnScreen);
    surface_->setAttribute(Qt::WA_NoSystemBackground);
#else
    surface_->setStyleSheet(QStringLiteral("background: black"));
#endif
    layout->addWidget(surface_, 1);
    errorLabel_ = new QLabel(this);
    errorLabel_->setStyleSheet(QStringLiteral("color: #ffb4b4"));
    errorLabel_->hide();
    layout->addWidget(errorLabel_);
    auto* controls = new QHBoxLayout();
    pauseButton_ = new QPushButton(QStringLiteral("Ⅱ"), this);
    pauseButton_->setToolTip(QStringLiteral("暂停视频"));
    seekSlider_ = new QSlider(Qt::Horizontal, this);
    seekSlider_->setObjectName(QStringLiteral("ffplaySeekSlider"));
    seekSlider_->setRange(0, 1000);
    timeLabel_ = new QLabel(QStringLiteral("0:00 / 0:00"), this);
    timeLabel_->setObjectName(QStringLiteral("ffplayTimeLabel"));
    timeLabel_->setStyleSheet(QStringLiteral("color: white"));
    controls->addWidget(pauseButton_);
    controls->addWidget(seekSlider_, 1);
    controls->addWidget(timeLabel_);
    speedSelector_ = new QComboBox(this);
    speedSelector_->setObjectName(QStringLiteral("ffplaySpeedSelector"));
    speedSelector_->setToolTip(QStringLiteral("播放倍速"));
    for (double rate : {0.25, 0.5, 1.0, 1.5, 2.0})
        speedSelector_->addItem(QStringLiteral("%1×").arg(rate), rate);
    speedSelector_->setCurrentIndex(2);
    controls->addWidget(speedSelector_);
    layout->addLayout(controls);
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
        pauseButton_->setText(playing_ ? QStringLiteral("Ⅱ") : QStringLiteral("▶"));
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
    auto* forward = new QPushButton(QStringLiteral("↗"), this);
    forward->setObjectName(QStringLiteral("videoForwardButton"));
    forward->setStyleSheet(QStringLiteral(
        "color:white;background:#363636;border:0;border-radius:20px;"
        "min-width:40px;min-height:40px;"));
    forward->setToolTip(QStringLiteral("转发视频"));
    connect(forward, &QPushButton::clicked, this, &MaiFfplayVideoDialog::forwardVideo);
    actions->addWidget(forward);
    auto* save = new QPushButton(QStringLiteral("↓"), this);
    save->setObjectName(QStringLiteral("videoSaveButton"));
    save->setStyleSheet(forward->styleSheet());
    save->setToolTip(QStringLiteral("保存视频副本"));
    connect(save, &QPushButton::clicked, this, &MaiFfplayVideoDialog::saveCopy);
    actions->addWidget(save);
    auto* more = new QPushButton(QStringLiteral("⋯"), this);
    more->setToolTip(QStringLiteral("更多操作"));
    more->setStyleSheet(forward->styleSheet());
    connect(more, &QPushButton::clicked, this, [this, more] {
        QMenu menu(this);
        QAction* copyPath = menu.addAction(QStringLiteral("复制文件路径"));
        QAction* openFolder = menu.addAction(QStringLiteral("打开所在文件夹"));
        QAction* chosen = menu.exec(more->mapToGlobal(more->rect().bottomLeft()));
        if (chosen == copyPath) QApplication::clipboard()->setText(path_);
        else if (chosen == openFolder)
            QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path_).absolutePath()));
    });
    actions->addWidget(more);
    layout->addLayout(actions);
}

void MaiFfplayVideoDialog::setForwardAction(std::function<void()> action) {
    forwardAction_ = std::move(action);
}

void MaiFfplayVideoDialog::forwardVideo() {
    if (forwardAction_) {
        forwardAction_();
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
    const QString value = QString::fromUtf8(title ? title : "Video");
    QMetaObject::invokeMethod(dialog, [dialog, value] { dialog->setWindowTitle(value); },
                              Qt::QueuedConnection);
}

void MaiFfplayVideoDialog::setSize(void* userData, int width, int height) {
    auto* dialog = static_cast<MaiFfplayVideoDialog*>(userData);
    QMetaObject::invokeMethod(dialog, [dialog, width, height] {
        dialog->resize(std::max(width, 640), std::max(height + 60, 420));
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
        pauseButton_->setText(playing_ ? QStringLiteral("暂停") : QStringLiteral("播放"));
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

void MaiFfplayVideoDialog::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
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

void MaiFfplayVideoDialog::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) close();
    else if (event->key() == Qt::Key_Space) sendCommand(QStringLiteral("pause"));
    else if (event->key() == Qt::Key_Left) sendCommand(QStringLiteral("seek_backward"));
    else if (event->key() == Qt::Key_Right) sendCommand(QStringLiteral("seek_forward"));
    else if (event->key() == Qt::Key_PageUp) sendCommand(QStringLiteral("next_chapter"));
    else if (event->key() == Qt::Key_PageDown) sendCommand(QStringLiteral("previous_chapter"));
    else QDialog::keyPressEvent(event);
}
