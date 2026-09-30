#include "ui/MessageImageLoader.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QImageReader>
#include <QLabel>
#include <QTemporaryDir>
#include <QThread>
#include <QWidget>

int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  QApplication app(argc, argv);
  QLabel label;
  label.setFixedSize(64, 64);
  label.show();
  MessageImageLoader::instance().loadInto(QString::fromLocal8Bit(argv[1]),
                                          QSize(64, 64), &label);
  QElapsedTimer timer;
  timer.start();
  bool presented = false;
  while (timer.elapsed() < 8000) {
    app.processEvents();
    QWidget *overlay =
        label.findChild<QWidget *>(QStringLiteral("maiGraphicsImageOverlay"));
    if (overlay && overlay->property("graphicsPresented").toBool()) {
      presented = true;
      break;
    }
    QThread::msleep(10);
  }
  if (!presented)
    return 1;

  QTemporaryDir temporary;
  if (!temporary.isValid())
    return 3;
  const QString svgPath = temporary.filePath(QStringLiteral("qt-supported.svg"));
  QFile svg(svgPath);
  if (!svg.open(QIODevice::WriteOnly))
    return 4;
  svg.write("<svg xmlns='http://www.w3.org/2000/svg' width='8' height='8'>"
            "<rect width='8' height='8' fill='red'/></svg>");
  svg.close();
  if (QImageReader(svgPath).read().isNull())
    return 5;

  QLabel unsupported;
  unsupported.setFixedSize(64, 64);
  unsupported.show();
  bool missing = false;
  MessageImageLoader::instance().loadInto(svgPath, QSize(64, 64), &unsupported,
                                           [&missing] { missing = true; });
  timer.restart();
  while (!missing && timer.elapsed() < 8000) {
    app.processEvents();
    QThread::msleep(10);
  }
  return missing && !unsupported.pixmap() ? 0 : 6;
}
