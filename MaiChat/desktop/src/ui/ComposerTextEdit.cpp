#include "ui/ComposerTextEdit.h"

#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QFileInfo>
#include <QMimeData>
#include <QResizeEvent>
#include <QUrl>
#include <utility>

#include "ui/UiZoom.h"

ComposerTextEdit::ComposerTextEdit(QWidget* parent) : QTextEdit(parent) { setAcceptDrops(true); }

void ComposerTextEdit::setMimeHandler(std::function<bool(const QMimeData*)> handler) {
    mimeHandler_ = std::move(handler);
}

void ComposerTextEdit::setCornerAction(QWidget* action) {
    cornerAction_ = action;
    if (!cornerAction_) return;
    cornerAction_->setParent(this);
    positionCornerAction();
}

void ComposerTextEdit::positionCornerAction() {
    if (!cornerAction_) return;
    const int inset = qMax(UiZoom::s(6), cornerAction_->width() / 5);
    cornerAction_->move(width() - cornerAction_->width() - inset,
                        height() - cornerAction_->height() - inset);
    cornerAction_->raise();
}

void ComposerTextEdit::resizeEvent(QResizeEvent* event) {
    QTextEdit::resizeEvent(event);
    positionCornerAction();
}

bool ComposerTextEdit::canInsertFromMimeData(const QMimeData* source) const {
    if (hasLocalFile(source)) return true;
    return QTextEdit::canInsertFromMimeData(source);
}

void ComposerTextEdit::insertFromMimeData(const QMimeData* source) {
    if (mimeHandler_ && mimeHandler_(source)) return;
    QTextEdit::insertFromMimeData(source);
}

void ComposerTextEdit::dragEnterEvent(QDragEnterEvent* event) {
    if (hasLocalFile(event->mimeData())) {
        event->acceptProposedAction();
        return;
    }
    QTextEdit::dragEnterEvent(event);
}

void ComposerTextEdit::dragMoveEvent(QDragMoveEvent* event) {
    if (hasLocalFile(event->mimeData())) {
        event->acceptProposedAction();
        return;
    }
    QTextEdit::dragMoveEvent(event);
}

bool ComposerTextEdit::hasLocalFile(const QMimeData* source) {
    if (!source || !source->hasUrls()) return false;
    for (const QUrl& url : source->urls()) {
        if (url.isLocalFile() && QFileInfo(url.toLocalFile()).isFile()) return true;
    }
    return false;
}
