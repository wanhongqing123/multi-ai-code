// 文件作用：桌面端共用界面；PdfPreviewDialog 负责对话框呈现、确认交互和结果回传，本文件给出其声明。
// 对外边界：本文件给出 PdfPreviewDialog 的类型和函数声明；参数、返回值和对象生命周期以此为准。
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
