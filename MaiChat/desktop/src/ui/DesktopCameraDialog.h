// 文件作用：Desktop 拍照预览与用户确认；普通界面和 Agent 工具复用同一入口。
#pragma once

#include <QDialog>
#include <QString>

class QCamera;
class QCameraImageCapture;
class QCameraViewfinder;
class QLabel;
class QPushButton;

class DesktopCameraDialog final : public QDialog {
public:
    // 返回新建 JPEG 绝对路径；取消返回空路径和空错误，无摄像头或失败返回错误。
    static QString capturePhoto(QWidget* parent, const QString& directory, QString* error);

    ~DesktopCameraDialog() override;

private:
    DesktopCameraDialog(QWidget* parent, const QString& destination);
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
    int pendingCapture_ = -1;
};
