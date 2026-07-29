#include "ui/RemoteInputCapture.h"

#include <QEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QWheelEvent>

namespace {

RemoteInput::MouseButton toRemoteButton(Qt::MouseButton button) {
    switch (button) {
        case Qt::RightButton: return RemoteInput::MouseButton::Right;
        case Qt::MiddleButton: return RemoteInput::MouseButton::Middle;
        default: return RemoteInput::MouseButton::Left;
    }
}

}  // namespace

RemoteInputCapture::RemoteInputCapture(RemoteInput::RemoteInputSender& sender, QObject* parent)
    : QObject(parent), sender_(sender) {}

void RemoteInputCapture::attachTo(QWidget* surface) {
    if (surface_ == surface) return;
    if (surface_) surface_->removeEventFilter(this);
    surface_ = surface;
    if (surface_) {
        surface_->installEventFilter(this);
        refreshContentRect();
    }
}

void RemoteInputCapture::setRemoteVideoSize(const QSize& size) {
    remoteVideoSize_ = size;
    refreshContentRect();
}

void RemoteInputCapture::setEnabled(bool enabled) {
    if (enabled_ == enabled) return;
    enabled_ = enabled;
    if (!enabled_) {
        // 关掉控制时必须让对端把一切放开：本地松手它是看不见的，
        // 不发这一条就会留下按住的键。
        pressedButtons_ = Qt::NoButton;
        sender_.queueReleaseAll();
    }
    if (surface_) {
        // 控制态下才需要键盘焦点，否则会抢走聊天输入框的按键。
        surface_->setFocusPolicy(enabled_ ? Qt::StrongFocus : Qt::NoFocus);
        if (enabled_) surface_->setFocus(Qt::OtherFocusReason);
    }
}

void RemoteInputCapture::refreshContentRect() {
    if (!surface_ || !remoteVideoSize_.isValid() || remoteVideoSize_.isEmpty()) {
        sender_.setContentRect(QRectF());
        return;
    }
    sender_.setContentRect(
        RemoteInput::fitContentRect(QSizeF(surface_->size()), QSizeF(remoteVideoSize_)));
}

bool RemoteInputCapture::mapPosition(const QPointF& pos, double* x, double* y) const {
    // 拖拽途中滑出画面区要钳到边界继续跟随；没按下时落在黑边上则不产生输入
    // （黑边不对应远端屏幕的任何位置，硬映射会变成点到屏幕边缘）。
    return pressedButtons_ != Qt::NoButton ? sender_.mapToNormalizedClamped(pos, x, y)
                                           : sender_.mapToNormalized(pos, x, y);
}

bool RemoteInputCapture::eventFilter(QObject* watched, QEvent* event) {
    if (watched == surface_ && event->type() == QEvent::Resize) {
        // 控件尺寸变了，黑边随之变化，必须重算，否则坐标整体偏移。
        refreshContentRect();
        return false;
    }

    if (!enabled_ || watched != surface_) return QObject::eventFilter(watched, event);

    switch (event->type()) {
        case QEvent::MouseMove: {
            auto* mouse = static_cast<QMouseEvent*>(event);
            double x = 0.0;
            double y = 0.0;
            if (mapPosition(mouse->localPos(), &x, &y)) sender_.queueMouseMove(x, y);
            return true;
        }
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonDblClick: {
            auto* mouse = static_cast<QMouseEvent*>(event);
            double x = 0.0;
            double y = 0.0;
            if (!mapPosition(mouse->localPos(), &x, &y)) return true;
            pressedButtons_ |= mouse->button();
            sender_.queueMouseButton(toRemoteButton(mouse->button()), true, x, y);
            // 双击不额外造事件：连着两次按下抬起，远端自己会判定成双击，
            // 我们再补一次只会点成三下。
            return true;
        }
        case QEvent::MouseButtonRelease: {
            auto* mouse = static_cast<QMouseEvent*>(event);
            // 只有我们确实发过按下的键才转发抬起：在黑边上点一下不产生按下，
            // 凭空补一个抬起会让远端收到没头没尾的事件。
            if (!pressedButtons_.testFlag(mouse->button())) return true;
            pressedButtons_ &= ~mouse->button();

            double x = 0.0;
            double y = 0.0;
            // 位置落在黑边上也照发——漏掉抬起会让远端一直按着，那正是悬空按键。
            sender_.mapToNormalizedClamped(mouse->localPos(), &x, &y);
            sender_.queueMouseButton(toRemoteButton(mouse->button()), false, x, y);
            return true;
        }
        case QEvent::Wheel: {
            auto* wheel = static_cast<QWheelEvent*>(event);
            double x = 0.0;
            double y = 0.0;
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
            const QPointF pos = wheel->position();
#else
            const QPointF pos = wheel->posF();
#endif
            if (!mapPosition(pos, &x, &y)) return true;
            sender_.queueWheel(wheel->angleDelta().y(), x, y);
            return true;
        }
        case QEvent::KeyPress:
        case QEvent::KeyRelease: {
            auto* key = static_cast<QKeyEvent*>(event);

            // 急停热键：本地吞掉，绝不转发给远端。
            // 控制期间鼠标可能被注入的动作带偏（同机自测时尤其明显），
            // 那时候连"控制"按钮都点不中，必须留一条纯键盘的退路。
            if (key->key() == Qt::Key_Q
                && key->modifiers().testFlag(Qt::ControlModifier)
                && key->modifiers().testFlag(Qt::AltModifier)
                && key->modifiers().testFlag(Qt::ShiftModifier)) {
                if (event->type() == QEvent::KeyPress) emit releaseControlRequested();
                return true;
            }

            if (key->isAutoRepeat()) {
                // 自动重复由远端系统自己产生，转发只会翻倍。
                return true;
            }
            const bool pressed = event->type() == QEvent::KeyPress;
            // nativeVirtualKey 是平台原生键码，正是注入端要的；
            // 拿 Qt::Key 转换会在符号键与布局差异上出错。
            const quint32 nativeKey = key->nativeVirtualKey();
            if (nativeKey != 0) sender_.queueKey(nativeKey, pressed);

            // 输入法上屏的文本没有对应键码，只能走文本通道。
            if (pressed && nativeKey == 0 && !key->text().isEmpty()) {
                sender_.queueText(key->text());
            }
            return true;
        }
        case QEvent::FocusOut:
            // 焦点跑了本地就收不到抬起事件了，先让对端全部放开。
            pressedButtons_ = Qt::NoButton;
            sender_.queueReleaseAll();
            return false;
        default:
            break;
    }
    return QObject::eventFilter(watched, event);
}
