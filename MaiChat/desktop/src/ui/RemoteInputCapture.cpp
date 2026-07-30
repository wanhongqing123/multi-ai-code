#include "ui/RemoteInputCapture.h"

#include <QDebug>
#include <QEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QTimer>
#include <QWheelEvent>

#include "remote/RemoteKeyMapping.h"

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
    : QObject(parent), sender_(sender) {
    // 控制期间每秒汇总一次采集情况——这是整条链路上唯一能回答"Qt 到底有没有
    // 收到鼠标事件"的地方：画面是 SDK 渲染的原生窗口，它若在上面盖了自己的
    // 子窗口把鼠标吃掉，事件过滤器就一次都不会触发，而下游看起来只是"没有
    // 输入产生"，两者从发送侧分不出来。
    // 定时器只在控制期间跑，平时不产生任何日志。
    traceTimer_ = new QTimer(this);
    traceTimer_->setInterval(1000);
    connect(traceTimer_, &QTimer::timeout, this, &RemoteInputCapture::flushTrace);
}

void RemoteInputCapture::attachTo(QWidget* surface) {
    if (surface_ == surface) return;
    // 换目标前先把上一块画面的鼠标跟踪还原，别把属性留在别人身上。
    endMouseTracking();
    if (surface_) surface_->removeEventFilter(this);
    surface_ = surface;
    if (surface_) {
        surface_->installEventFilter(this);
        if (enabled_) beginMouseTracking();
        refreshContentRect();
    }
}

void RemoteInputCapture::beginMouseTracking() {
    if (!surface_ || mouseTrackingApplied_) return;
    // 不开这个，Qt 只在**按住键**时才投递 MouseMove（隐式抓取），悬空移动
    // 鼠标一个事件都不产生——远端光标纹丝不动，而点击、滚轮、键盘全都正常，
    // 看起来特别像"链路通了但鼠标没实现"。实测踩过。
    mouseTrackingRestore_ = surface_->hasMouseTracking();
    surface_->setMouseTracking(true);
    mouseTrackingApplied_ = true;
}

void RemoteInputCapture::endMouseTracking() {
    if (surface_ && mouseTrackingApplied_) surface_->setMouseTracking(mouseTrackingRestore_);
    // surface_ 可能已经析构（QPointer 置空），标志位无论如何都要清掉。
    mouseTrackingApplied_ = false;
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
    // 同理只在控制期间开鼠标跟踪：平时开着会让光标每划过画面一次就走一遍
    // 事件过滤器，纯属白烧。
    if (enabled_) {
        beginMouseTracking();
    } else {
        endMouseTracking();
    }

    if (enabled_) {
        qInfo().noquote()
            << QStringLiteral(
                   "[remote-input] capture: control started (surface=%1, mouseTracking=%2, "
                   "contentRect=%3)")
                   .arg(surface_ ? QStringLiteral("bound") : QStringLiteral("none"))
                   .arg(surface_ && surface_->hasMouseTracking() ? QStringLiteral("on")
                                                                 : QStringLiteral("off"))
                   .arg(sender_.contentRect().isEmpty()
                            ? QStringLiteral("empty (all coordinate mapping will fail)")
                            : QStringLiteral("ok"));
        logGeometry(QStringLiteral("control started"));
        traceTimer_->start();
    } else {
        flushTrace();
        traceTimer_->stop();
        qInfo().noquote() << QStringLiteral("[remote-input] capture: control stopped");
    }
}

void RemoteInputCapture::flushTrace() {
    const bool sawAnything = traceMoveSeen_ > 0 || traceButtonSeen_ > 0 || traceWheelSeen_ > 0
                             || traceKeySeen_ > 0;
    // 一个事件都没收到时也要说话，而且要说得明确——这正是"SDK 把鼠标吃了"的
    // 特征，日志里空着的话会被当成"用户没动鼠标"。
    if (!sawAnything) {
        qInfo().noquote() << QStringLiteral(
            "[remote-input] capture: no mouse/keyboard event reached Qt in the last 1s "
            "(if you were moving the mouse over the video area, the SDK render child window "
            "is swallowing the events before Qt sees them)");
        return;
    }
    QString sample;
    if (traceMoveQueued_ > 0) {
        sample = QStringLiteral(" last=(%1,%2)px->(%3,%4)")
                     .arg(traceLastLocal_.x(), 0, 'f', 0)
                     .arg(traceLastLocal_.y(), 0, 'f', 0)
                     .arg(traceLastNormX_, 0, 'f', 4)
                     .arg(traceLastNormY_, 0, 'f', 4);
    }
    if (traceMoveSeen_ > traceMoveQueued_) {
        // 丢弃样本 + 当前内容区一起打，好判断到底是真黑边还是算错了。
        const QRectF rect = sender_.contentRect();
        sample += QStringLiteral(" dropped-at=(%1,%2)px contentRect=(%3,%4 %5x%6)")
                      .arg(traceLastDroppedLocal_.x(), 0, 'f', 0)
                      .arg(traceLastDroppedLocal_.y(), 0, 'f', 0)
                      .arg(rect.x(), 0, 'f', 1)
                      .arg(rect.y(), 0, 'f', 1)
                      .arg(rect.width(), 0, 'f', 1)
                      .arg(rect.height(), 0, 'f', 1);
    }
    qInfo().noquote()
        << QStringLiteral(
               "[remote-input] capture: moves seen=%1 queued=%2 dropped-letterbox=%3 | "
               "buttons=%4 wheel=%5 keys=%6%7")
               .arg(traceMoveSeen_)
               .arg(traceMoveQueued_)
               .arg(traceMoveSeen_ - traceMoveQueued_)
               .arg(traceButtonSeen_)
               .arg(traceWheelSeen_)
               .arg(traceKeySeen_)
               .arg(sample);
    traceMoveSeen_ = 0;
    traceMoveQueued_ = 0;
    traceButtonSeen_ = 0;
    traceWheelSeen_ = 0;
    traceKeySeen_ = 0;
}

void RemoteInputCapture::refreshContentRect() {
    if (!surface_ || !remoteVideoSize_.isValid() || remoteVideoSize_.isEmpty()) {
        sender_.setContentRect(QRectF());
        return;
    }
    sender_.setContentRect(
        RemoteInput::fitContentRect(QSizeF(surface_->size()), QSizeF(remoteVideoSize_)));
    logGeometry(QStringLiteral("content rect"));
}

// 坐标偏移只可能来自这三个数的关系，把它们原样打出来就能手算复核，
// 不用再让人重试一轮去猜。
void RemoteInputCapture::logGeometry(const QString& reason) const {
    if (!surface_) return;
    const QRectF rect = sender_.contentRect();
    qInfo().noquote()
        << QStringLiteral("[remote-input] capture geometry (%1): widget=%2x%3 video=%4x%5 "
                          "(aspect %6) contentRect=(%7,%8 %9x%10)")
               .arg(reason)
               .arg(surface_->width())
               .arg(surface_->height())
               .arg(remoteVideoSize_.width())
               .arg(remoteVideoSize_.height())
               .arg(remoteVideoSize_.height() > 0
                        ? QString::number(static_cast<double>(remoteVideoSize_.width())
                                              / remoteVideoSize_.height(),
                                          'f', 4)
                        : QStringLiteral("n/a"))
               .arg(rect.x(), 0, 'f', 1)
               .arg(rect.y(), 0, 'f', 1)
               .arg(rect.width(), 0, 'f', 1)
               .arg(rect.height(), 0, 'f', 1);
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
            ++traceMoveSeen_;
            if (mapPosition(mouse->localPos(), &x, &y)) {
                sender_.queueMouseMove(x, y);
                ++traceMoveQueued_;
                traceLastLocal_ = mouse->localPos();
                traceLastNormX_ = x;
                traceLastNormY_ = y;
            } else {
                // 被判成黑边而丢弃的点也要留一个样本：黑边算错时，正是这些点
                // 能证明"我明明在画面上"。
                traceLastDroppedLocal_ = mouse->localPos();
            }
            return true;
        }
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonDblClick: {
            auto* mouse = static_cast<QMouseEvent*>(event);
            double x = 0.0;
            double y = 0.0;
            ++traceButtonSeen_;
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
            ++traceWheelSeen_;
            if (!mapPosition(pos, &x, &y)) return true;
            sender_.queueWheel(wheel->angleDelta().y(), x, y);
            return true;
        }
        case QEvent::KeyPress:
        case QEvent::KeyRelease: {
            auto* key = static_cast<QKeyEvent*>(event);
            ++traceKeySeen_;

            // 急停热键：本地吞掉，绝不转发给远端。
            // 控制期间鼠标可能被注入的动作带偏（同机自测时尤其明显），
            // 那时候连"控制"按钮都点不中，必须留一条纯键盘的退路。
            if (key->key() == Qt::Key_Q
                && key->modifiers().testFlag(Qt::ControlModifier)
                && key->modifiers().testFlag(Qt::AltModifier)
                && key->modifiers().testFlag(Qt::ShiftModifier)) {
                if (event->type() == QEvent::KeyPress) {
                    // 修饰键的按下事件会先于 Q 到达，可能已经发给远端；急停时
                    // 主动全部抬起，不能只吞掉 Q 后寄希望于后续 KeyRelease。
                    pressedButtons_ = Qt::NoButton;
                    sender_.queueReleaseAll();
                    emit releaseControlRequested();
                }
                return true;
            }

            if (key->isAutoRepeat()) {
                // 自动重复由远端系统自己产生，转发只会翻倍。
                return true;
            }
            const bool pressed = event->type() == QEvent::KeyPress;
            // 协议统一使用 Windows VK 作为平台无关的物理键标识。macOS 的
            // nativeVirtualKey 是 CGKeyCode（A 恰好为 0），直接发送会让
            // Mac→Windows 的字母键和快捷键失效。
            quint32 canonicalKey = RemoteInput::canonicalKeyCodeFromQt(
                key->key(), key->modifiers().testFlag(Qt::KeypadModifier));
#ifdef Q_OS_WIN
            // Windows 原生值能保留左右修饰键等信息，优先使用；Qt 映射兜底。
            if (key->nativeVirtualKey() != 0) canonicalKey = key->nativeVirtualKey();
#endif
            if (canonicalKey != 0) sender_.queueKey(canonicalKey, pressed);

            // 输入法上屏的文本没有对应键码，只能走文本通道。
            if (pressed && canonicalKey == 0 && !key->text().isEmpty()) {
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
