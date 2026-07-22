#include "ui/ImagePreviewDialog.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QVBoxLayout>

ImagePreviewDialog::ImagePreviewDialog(const QString& imagePath, QWidget* parent)
    : QDialog(parent), image_(imagePath) {
    setWindowTitle(QStringLiteral("图片预览"));
    setModal(true);
    setStyleSheet(QStringLiteral("QDialog { background: #08090b; } QLabel { color: white; }"));

    imageLabel_ = new QLabel(this);
    imageLabel_->setAlignment(Qt::AlignCenter);
    imageLabel_->setMinimumSize(320, 240);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(imageLabel_);
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
    imageLabel_->setPixmap(image_.scaled(imageLabel_->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
}

