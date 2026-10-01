#include "ui/MaiFfplayVideoDialog.h"

#include <QCloseEvent>
#include <QCoreApplication>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMetaObject>
#include <QPushButton>
#include <QResizeEvent>
#include <QShowEvent>
#include <QSlider>
#include <QThread>
#include <QVBoxLayout>

#include <algorithm>
#include <atomic>

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
    if (relative.endsWith(QStringLiteral("libmaiagent_obs_metal.so")))
        return base + QStringLiteral("/maiagent/Graphics/libmaiagent_obs_metal.so");
    return base + QStringLiteral("/maiagent/Graphics/data");
#endif
}

}  // namespace

MaiFfplayVideoDialog::MaiFfplayVideoDialog(const QString& path, QWidget* parent)
    : QDialog(parent), path_(path) {
    setWindowTitle(QFileInfo(path).fileName());
    setMinimumSize(640, 420);
    setStyleSheet(QStringLiteral("QDialog { background: #10151f; }"));
    auto* layout = new QVBoxLayout(this);
    surface_ = new QWidget(this);
    surface_->setObjectName(QStringLiteral("ffplayVideoSurface"));
    surface_->setAttribute(Qt::WA_NativeWindow);
#if defined(Q_OS_WIN)
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
    pauseButton_ = new QPushButton(QStringLiteral("暂停"), this);
    seekSlider_ = new QSlider(Qt::Horizontal, this);
    seekSlider_->setRange(0, 1000);
    controls->addWidget(pauseButton_);
    controls->addWidget(seekSlider_, 1);
    layout->addLayout(controls);
    connect(pauseButton_, &QPushButton::clicked, this, [this] {
        sendCommand(QStringLiteral("pause"));
    });
    connect(seekSlider_, &QSlider::sliderReleased, this, [this] {
        seekPercent(seekSlider_->value() / 1000.0);
    });
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

bool MaiFfplayVideoDialog::startPlayback() {
    if (started_ || !QFileInfo(path_).isFile()) return false;
    if (!sActiveDialog.isNull() && sActiveDialog != this) sActiveDialog->close();
    const QString backend = bundledResource(
        QStringLiteral("/../Frameworks/libmaiagent_obs_metal.so"));
    const QString effects = bundledResource(
        QStringLiteral("/../Resources/MaiAgentGraphics"));
    if (!QFileInfo::exists(backend) ||
        !QFileInfo::exists(effects + QStringLiteral("/default.effect"))) return false;
    if (sPresenterUsers == 0) {
        const QByteArray modulePath = backend.toUtf8();
        const QByteArray effectPath = effects.toUtf8();
        if (!maiGraphicsPresenterStart(modulePath.constData(), effectPath.constData(),
                                       onFramePresented, nullptr)) return false;
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
    playbackFinished_ = false;
    sActiveDialog = this;
    playbackThread_ = std::thread([this, sourcePath] {
        char name[] = "ffplay";
        char* arguments[] = {name, const_cast<char*>(sourcePath.constData())};
        const int result = maiFfplayRun(&host_, 2, arguments);
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
    else QDialog::keyPressEvent(event);
}
