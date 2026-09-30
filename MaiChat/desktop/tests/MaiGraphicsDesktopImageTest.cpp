#include "ui/MessageImageLoader.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QLabel>
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
  while (timer.elapsed() < 8000) {
    app.processEvents();
    QWidget *overlay =
        label.findChild<QWidget *>(QStringLiteral("maiGraphicsImageOverlay"));
    if (overlay && overlay->property("graphicsPresented").toBool())
      return 0;
    QThread::msleep(10);
  }
  return 1;
}
