#include "ui/ImagePreviewDialog.h"

#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QScreen>
#include <QSizePolicy>
#include <QVBoxLayout>
#include <QWindow>

ImagePreviewDialog::ImagePreviewDialog(const QString& imagePath, QWidget* parent)
    : QDialog(parent), image_(imagePath) {
    setWindowTitle(QStringLiteral("图片预览"));
    setModal(true);
    setSizeGripEnabled(true);
    setStyleSheet(QStringLiteral("QDialog { background: #08090b; } QLabel { color: white; }"));

    imageLabel_ = new QLabel(this);
    imageLabel_->setAlignment(Qt::AlignCenter);
    imageLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    // 图片只是展示内容；让点击直接落到对话框，由对话框统一关闭预览。
    imageLabel_->setAttribute(Qt::WA_TransparentForMouseEvents);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(imageLabel_);

    if (!image_.isNull()) {
        QSize initialSize = (QSizeF(image_.size()) / image_.devicePixelRatioF()).toSize();
        QScreen* targetScreen = parent && parent->windowHandle()
            ? parent->windowHandle()->screen()
            : QGuiApplication::primaryScreen();
        if (targetScreen) {
            const QSize available = targetScreen->availableGeometry().size() - QSize(80, 100);
            if (initialSize.width() > available.width() || initialSize.height() > available.height()) {
                initialSize.scale(available, Qt::KeepAspectRatio);
            }
        }
        resize(initialSize);
    } else {
        resize(320, 240);
    }
    updateImage();
}

void ImagePreviewDialog::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape || event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        accept();
        return;
    }
    QDialog::keyPressEvent(event);
}

void ImagePreviewDialog::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        accept();
        return;
    }
    QDialog::mousePressEvent(event);
}

void ImagePreviewDialog::resizeEvent(QResizeEvent* event) {
    QDialog::resizeEvent(event);
    updateImage();
}

void ImagePreviewDialog::updateImage() {
    if (image_.isNull()) {
        imageLabel_->setText(QStringLiteral("图片无法加载"));
        return;
    }
    // 高分屏（DPR>1）按物理分辨率缩放并声明 DPR，否则预览图被绘制层二次放大而发虚。
    const qreal dpr = devicePixelRatioF();
    QPixmap scaled = image_.scaled((QSizeF(imageLabel_->size()) * dpr).toSize(),
                                   Qt::KeepAspectRatio, Qt::SmoothTransformation);
    scaled.setDevicePixelRatio(dpr);
    imageLabel_->setPixmap(scaled);
}
