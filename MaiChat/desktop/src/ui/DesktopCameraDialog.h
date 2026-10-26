// 文件作用：Desktop 拍照预览与用户确认；普通界面和 Agent 工具复用同一入口。
#pragma once

#include <QDialog>
#include <QString>

#include <atomic>

class QCamera;
class QCameraImageCapture;
class QCameraViewfinder;
class QLabel;
class QPushButton;

class DesktopCameraDialog final : public QDialog {
public:
    // 返回新建 JPEG 绝对路径；取消返回空路径和空错误，无摄像头或失败返回错误。
    static QString capturePhoto(QWidget* parent, const QString& directory, QString* error,
                                const std::atomic<bool>* canceled = nullptr);
    static bool openPreview(QWidget* parent, const QString& sessionId,
                            const QString& directory, QString* error);
    static bool closePreview(QWidget* parent, const QString& sessionId);
    static bool isPreviewOpen(QWidget* parent, const QString& sessionId);
    static QString previewError(QWidget* parent, const QString& sessionId);
    static QString lastPreviewPhoto(QWidget* parent, const QString& sessionId);

    ~DesktopCameraDialog() override;

private:
    DesktopCameraDialog(QWidget* parent, const QString& destination, bool persistent = false);
    void reject() override;
    void takePhoto();

    QCamera* camera_ = nullptr;
    QCameraImageCapture* capture_ = nullptr;
    QCameraViewfinder* viewfinder_ = nullptr;
    QLabel* status_ = nullptr;
    QPushButton* shutter_ = nullptr;
    QString destination_;
    QString savedPath_;
    QString pendingPath_;
    QString error_;
    bool persistent_ = false;
    int pendingCapture_ = -1;
};
