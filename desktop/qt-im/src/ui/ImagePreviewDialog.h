#pragma once

#include <QDialog>
#include <QLabel>
#include <QPixmap>
#include <QString>

class ImagePreviewDialog final : public QDialog {
public:
    explicit ImagePreviewDialog(const QString& imagePath, QWidget* parent = nullptr);

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void updateImage();

    QLabel* imageLabel_ = nullptr;
    QPixmap image_;
};

