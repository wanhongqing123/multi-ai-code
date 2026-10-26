// 文件作用：用 Qt5 摄像头预览拍摄一张图片；失败不返回不存在的文件。
#include "ui/DesktopCameraDialog.h"

#include <QCamera>
#include <QApplication>
#include <QCameraImageCapture>
#include <QCameraInfo>
#include <QCameraViewfinder>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHash>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>

namespace {
struct CameraPreviewState {
    QPointer<DesktopCameraDialog> dialog;
    QString sessionId;
    QString directory;
    QString lastPhoto;
    QUuid generation;
};

QHash<QWidget*, CameraPreviewState>& activePreviews() {
    static QHash<QWidget*, CameraPreviewState> previews;
    return previews;
}
}

QString DesktopCameraDialog::capturePhoto(QWidget* parent, const QString& directory,
                                           QString* error, const std::atomic<bool>* canceled) {
    if (error) error->clear();
    if (canceled && canceled->load(std::memory_order_relaxed)) return {};
    if (QCameraInfo::availableCameras().isEmpty()) {
        if (error) *error = QStringLiteral("没有可用的摄像头");
        return {};
    }
    if (!QDir(directory).exists() || !QFileInfo(directory).isWritable()) {
        if (error) *error = QStringLiteral("当前工作目录不可写，无法保存照片");
        return {};
    }
    DesktopCameraDialog dialog(parent, directory);
    QTimer cancellation;
    if (canceled != nullptr) {
        cancellation.setInterval(100);
        QObject::connect(&cancellation, &QTimer::timeout, &dialog, [&dialog, canceled] {
            if (canceled->load(std::memory_order_relaxed)) dialog.reject();
        });
        cancellation.start();
    }
    if (dialog.exec() != QDialog::Accepted) {
        if (error) *error = dialog.error_;
        return {};
    }
    return dialog.savedPath_;
}

bool DesktopCameraDialog::openPreview(QWidget* parent, const QString& sessionId,
                                      const QString& directory, QString* error) {
    if (error) error->clear();
    if (!parent || sessionId.isEmpty()) {
        if (error) *error = QStringLiteral("MaiChat 窗口不可用");
        return false;
    }
    const CameraPreviewState current = activePreviews().value(parent);
    if (current.dialog && current.dialog->isVisible()) {
        if (current.sessionId != sessionId) {
            if (error) *error = QStringLiteral("另一段会话正在使用摄像头");
            return false;
        }
        current.dialog->raise();
        current.dialog->activateWindow();
        return true;
    }
    if (QCameraInfo::availableCameras().isEmpty()) {
        if (error) *error = QStringLiteral("没有可用的摄像头");
        return false;
    }
    if (!QDir(directory).exists() || !QFileInfo(directory).isWritable()) {
        if (error) *error = QStringLiteral("当前工作目录不可写，无法保存照片");
        return false;
    }
    auto* dialog = new DesktopCameraDialog(parent, directory, true);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    const QUuid generation = QUuid::createUuid();
    activePreviews().insert(parent, {dialog, sessionId, directory, {}, generation});
    QObject::connect(dialog, &QObject::destroyed, parent, [parent, generation] {
        auto preview = activePreviews().find(parent);
        if (preview != activePreviews().end() && preview->generation == generation)
            preview->dialog = nullptr;
    });
    QObject::connect(parent, &QObject::destroyed, qApp, [parent] {
        activePreviews().remove(parent);
    });
    dialog->show();
    return true;
}

bool DesktopCameraDialog::closePreview(QWidget* parent, const QString& sessionId) {
    const CameraPreviewState state = activePreviews().value(parent);
    const QPointer<DesktopCameraDialog> dialog = state.dialog;
    if (state.sessionId != sessionId || !dialog || !dialog->isVisible()) return false;
    dialog->reject();
    return true;
}

bool DesktopCameraDialog::isPreviewOpen(QWidget* parent, const QString& sessionId) {
    const CameraPreviewState state = activePreviews().value(parent);
    const QPointer<DesktopCameraDialog> dialog = state.sessionId == sessionId ? state.dialog : nullptr;
    return dialog && dialog->isVisible();
}

QString DesktopCameraDialog::previewError(QWidget* parent, const QString& sessionId) {
    const CameraPreviewState state = activePreviews().value(parent);
    return state.sessionId == sessionId && state.dialog ? state.dialog->error_ : QString();
}

QString DesktopCameraDialog::lastPreviewPhoto(QWidget* parent, const QString& sessionId) {
    const CameraPreviewState state = activePreviews().value(parent);
    const QString path = state.sessionId == sessionId ? state.lastPhoto : QString();
    return QFileInfo(path).isFile() ? path : QString();
}

DesktopCameraDialog::DesktopCameraDialog(QWidget* parent, const QString& destination, bool persistent)
    : QDialog(parent), destination_(destination), persistent_(persistent) {
    setObjectName(QStringLiteral("desktopCameraDialog"));
    setWindowTitle(QStringLiteral("拍照"));
    setMinimumSize(640, 520);
    auto* layout = new QVBoxLayout(this);
    viewfinder_ = new QCameraViewfinder(this);
    viewfinder_->setObjectName(QStringLiteral("desktopCameraPreview"));
    layout->addWidget(viewfinder_, 1);
    status_ = new QLabel(QStringLiteral("正在打开摄像头…"), this);
    layout->addWidget(status_);
    auto* actions = new QHBoxLayout;
    actions->addStretch();
    auto* cancel = new QPushButton(persistent ? QStringLiteral("关闭") : QStringLiteral("取消"), this);
    shutter_ = new QPushButton(QStringLiteral("拍照"), this);
    shutter_->setObjectName(QStringLiteral("desktopCameraShutter"));
    shutter_->setEnabled(false);
    actions->addWidget(cancel);
    actions->addWidget(shutter_);
    layout->addLayout(actions);
    connect(cancel, &QPushButton::clicked, this, &DesktopCameraDialog::reject);
    connect(shutter_, &QPushButton::clicked, this, &DesktopCameraDialog::takePhoto);

    camera_ = new QCamera(QCameraInfo::defaultCamera(), this);
    camera_->setCaptureMode(QCamera::CaptureStillImage);
    camera_->setViewfinder(viewfinder_);
    connect(camera_, QOverload<QCamera::Error>::of(&QCamera::error), this,
            [this](QCamera::Error) {
                error_ = camera_->errorString().isEmpty() ?
                    QStringLiteral("摄像头无法打开，请检查系统权限") : camera_->errorString();
                status_->setText(error_);
                shutter_->setEnabled(false);
            });
    capture_ = new QCameraImageCapture(camera_, this);
    capture_->setCaptureDestination(QCameraImageCapture::CaptureToFile);
    connect(capture_, &QCameraImageCapture::readyForCaptureChanged, this,
            [this](bool ready) {
                if (pendingCapture_ < 0) shutter_->setEnabled(ready);
                if (ready && pendingCapture_ < 0) status_->setText(QStringLiteral("准备就绪"));
            });
    connect(capture_, &QCameraImageCapture::imageSaved, this,
            [this](int id, const QString& path) {
                if (id != pendingCapture_) return;
                if (!QFileInfo(path).isFile() || QFileInfo(path).size() <= 0) {
                    error_ = QStringLiteral("照片未能保存，请重试");
                    status_->setText(error_);
                    QFile::remove(pendingPath_);
                    pendingPath_.clear();
                    pendingCapture_ = -1;
                    shutter_->setEnabled(capture_->isReadyForCapture());
                    return;
                }
                savedPath_ = path;
                if (persistent_) activePreviews()[parentWidget()].lastPhoto = path;
                pendingPath_.clear();
                pendingCapture_ = -1;
                camera_->stop();
                accept();
            });
    connect(capture_, QOverload<int, QCameraImageCapture::Error, const QString&>::of(
                &QCameraImageCapture::error), this,
            [this](int id, QCameraImageCapture::Error, const QString& message) {
                if (id != pendingCapture_) return;
                error_ = message.isEmpty() ? QStringLiteral("拍照失败") : message;
                status_->setText(error_);
                QFile::remove(pendingPath_);
                pendingPath_.clear();
                pendingCapture_ = -1;
                shutter_->setEnabled(capture_->isReadyForCapture());
            });
    camera_->start();
}

DesktopCameraDialog::~DesktopCameraDialog() {
    if (camera_) camera_->stop();
}

void DesktopCameraDialog::reject() {
    if (pendingCapture_ >= 0) capture_->cancelCapture();
    if (!pendingPath_.isEmpty()) QFile::remove(pendingPath_);
    if (!savedPath_.isEmpty()) QFile::remove(savedPath_);
    if (camera_) camera_->stop();
    QDialog::reject();
}

void DesktopCameraDialog::takePhoto() {
    if (!capture_ || !capture_->isReadyForCapture() || pendingCapture_ >= 0) return;
    const QString path = QDir(destination_).filePath(
        QStringLiteral("camera-%1.jpg").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    pendingPath_ = path;
    shutter_->setEnabled(false);
    status_->setText(QStringLiteral("正在保存照片…"));
    pendingCapture_ = capture_->capture(path);
    if (pendingCapture_ >= 0) return;
    error_ = capture_->errorString().isEmpty() ? QStringLiteral("拍照失败") : capture_->errorString();
    status_->setText(error_);
    QFile::remove(pendingPath_);
    pendingPath_.clear();
    shutter_->setEnabled(capture_->isReadyForCapture());
}
