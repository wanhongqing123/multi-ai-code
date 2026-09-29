#pragma once

#include <QString>
#ifdef Q_OS_WIN
#include <QImage>
#endif

class QWidget;

// Verify the local artifact before opening it in the in-app PDF viewer.
bool isPreviewablePdfFile(const QString &path, QString *error = nullptr);
void showPdfPreview(QWidget *parent, const QString &path,
                    const QString &displayName = {});

#ifdef Q_OS_WIN
struct PdfPreviewPage {
  QImage image;
  int pageCount = 0;
  QString error;
};
PdfPreviewPage renderWindowsPdfPage(const QString &path, int pageIndex);
#endif
