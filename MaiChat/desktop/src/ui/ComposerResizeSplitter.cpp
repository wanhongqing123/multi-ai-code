#include "ui/ComposerResizeSplitter.h"

#include <QEvent>
#include <QHideEvent>
#include <QMouseEvent>
#include <QSplitterHandle>

namespace {

class ComposerResizeHandle final : public QSplitterHandle {
  public:
    explicit ComposerResizeHandle(QSplitter *parent) : QSplitterHandle(Qt::Vertical, parent) {
        unsetCursor();
    }

  protected:
    void enterEvent(QEvent *event) override {
        setCursor(Qt::SplitVCursor);
        QSplitterHandle::enterEvent(event);
    }

    void leaveEvent(QEvent *event) override {
        unsetCursor();
        QSplitterHandle::leaveEvent(event);
    }

    void hideEvent(QHideEvent *event) override {
        unsetCursor();
        QSplitterHandle::hideEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override {
        QSplitterHandle::mouseReleaseEvent(event);
        if (!rect().contains(event->pos()))
            unsetCursor();
    }

    bool event(QEvent *event) override {
        if (event->type() == QEvent::UngrabMouse || event->type() == QEvent::WindowDeactivate)
            unsetCursor();
        return QSplitterHandle::event(event);
    }
};

} // namespace

ComposerResizeSplitter::ComposerResizeSplitter(QWidget *parent) : QSplitter(Qt::Vertical, parent) {
    setHandleWidth(1);
}

QSplitterHandle *ComposerResizeSplitter::createHandle() { return new ComposerResizeHandle(this); }
