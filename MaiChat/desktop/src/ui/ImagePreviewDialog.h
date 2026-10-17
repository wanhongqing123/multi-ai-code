// 文件作用：桌面端共用界面；ImagePreviewDialog 负责对话框呈现、确认交互和结果回传，本文件给出其声明。
// 类型关系：ImagePreviewDialog final : public QDialog；审阅时可据此追踪父类、接口或回调契约。
// 对外边界：本文件给出 ImagePreviewDialog 的类型和函数声明；参数、返回值和对象生命周期以此为准。
#pragma once

#include <QDialog>
#include <QLabel>
#include <QPixmap>
#include <QString>
#include <functional>

class QCloseEvent;

class ImagePreviewDialog final : public QDialog {
public:
    explicit ImagePreviewDialog(const QString& imagePath, QWidget* parent = nullptr);
    explicit ImagePreviewDialog(const QPixmap& image, QWidget* parent = nullptr);
    void setCloseHandler(std::function<bool()> handler);
    const QPixmap& sourceImage() const { return image_; }

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private:
    void updateImage();

    QLabel* imageLabel_ = nullptr;
    QPixmap image_;
    std::function<bool()> closeHandler_;
};
