#include <QtTest/QtTest>

#include <QFile>
#include <QPainter>
#include <QPdfWriter>
#include <QTemporaryDir>

#ifdef Q_OS_WIN
#include <QDialog>
#include <QLabel>
#endif

#include "ui/PdfPreviewDialog.h"

class PdfPreviewDialogTest : public QObject {
  Q_OBJECT

private slots:
  void validatesLocalPdfBeforePreview();
#ifdef Q_OS_WIN
  void opensPdfInsideApplication();
  void closesWhilePageIsLoading();
#endif
};

void PdfPreviewDialogTest::validatesLocalPdfBeforePreview() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  QString error;
  const QString path = directory.filePath(QStringLiteral("报告.pdf"));
  QVERIFY(!isPreviewablePdfFile(path, &error));
  QVERIFY(!error.isEmpty());

  QFile file(path);
  QVERIFY(file.open(QIODevice::WriteOnly));
  QCOMPARE(file.write("%PDF-1.4\n%%EOF\n"), qint64(15));
  file.close();
  QVERIFY(isPreviewablePdfFile(path, &error));
  QVERIFY(error.isEmpty());
  const QString cached = directory.filePath(QStringLiteral("cached-file"));
  QVERIFY(QFile::copy(path, cached));
  QVERIFY(isPreviewablePdfFile(cached));

  QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
  QCOMPARE(file.write("not a PDF\n"), qint64(10));
  file.close();
  QVERIFY(!isPreviewablePdfFile(path, &error));
  QVERIFY(!error.isEmpty());
}

#ifdef Q_OS_WIN
void PdfPreviewDialogTest::opensPdfInsideApplication() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString path = directory.filePath(QStringLiteral("preview.pdf"));
  QFile file(path);
  QVERIFY(file.open(QIODevice::WriteOnly));
  {
    QPdfWriter writer(&file);
    QPainter painter(&writer);
    painter.drawText(QPoint(100, 100), QStringLiteral("PDF preview smoke"));
    painter.end();
  }
  file.close();
  QVERIFY(isPreviewablePdfFile(path));

  showPdfPreview(nullptr, path, QStringLiteral("Friendly PDF"));
  QDialog *dialog = nullptr;
  for (QWidget *widget : QApplication::topLevelWidgets()) {
    if (widget->windowTitle() == QStringLiteral("Friendly PDF")) {
      dialog = qobject_cast<QDialog *>(widget);
      break;
    }
  }
  QVERIFY(dialog != nullptr);
  auto *pageLabel =
      dialog->findChild<QLabel *>(QStringLiteral("pdfPreviewPageLabel"));
  auto *pageImage =
      dialog->findChild<QLabel *>(QStringLiteral("pdfPreviewPageImage"));
  QVERIFY(pageLabel != nullptr);
  QVERIFY(pageImage != nullptr);
  QTRY_COMPARE_WITH_TIMEOUT(pageLabel->text(), QStringLiteral("1 / 1"), 10000);
  QVERIFY(pageImage->pixmap() != nullptr && !pageImage->pixmap()->isNull());
  dialog->close();
}

void PdfPreviewDialogTest::closesWhilePageIsLoading() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString path = directory.filePath(QStringLiteral("closing.pdf"));
  QFile file(path);
  QVERIFY(file.open(QIODevice::WriteOnly));
  {
    QPdfWriter writer(&file);
    QPainter painter(&writer);
    painter.drawText(QPoint(100, 100), QStringLiteral("Close while loading"));
    painter.end();
  }
  file.close();
  showPdfPreview(nullptr, path);
  for (QWidget *widget : QApplication::topLevelWidgets()) {
    if (widget->windowTitle() != QStringLiteral("closing.pdf"))
      continue;
    auto *dialog = qobject_cast<QDialog *>(widget);
    QVERIFY(dialog != nullptr);
    dialog->close();
    QTest::qWait(150);
    return;
  }
  QFAIL("PDF preview window was not created");
}
#endif

QTEST_MAIN(PdfPreviewDialogTest)
#include "PdfPreviewDialogTest.moc"
