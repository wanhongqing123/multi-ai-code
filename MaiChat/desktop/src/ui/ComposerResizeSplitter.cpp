// 文件作用：桌面端共用界面；ComposerResizeSplitter 负责输入区与消息区的可调整分隔布局，本文件给出其实现。
// 主要入口：createHandle。
// 实现边界：与 ComposerResizeSplitter 的公开声明配套，调用方通过相应接口进入。
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
