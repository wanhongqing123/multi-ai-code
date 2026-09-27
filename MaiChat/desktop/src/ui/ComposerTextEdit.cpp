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

void ComposerTextEdit::setLeadingAction(QWidget* action) {
    leadingAction_ = action;
    if (!leadingAction_) return;
    leadingAction_->setParent(this);
    positionCornerAction();
}

void ComposerTextEdit::setLeadingHint(QWidget* hint) {
    leadingHint_ = hint;
    if (!leadingHint_) return;
    leadingHint_->setParent(this);
    positionCornerAction();
}

void ComposerTextEdit::setSecondaryLeadingAction(QWidget* action) {
    secondaryLeadingAction_ = action;
    if (!action) return;
    action->setParent(this);
    positionCornerAction();
}

void ComposerTextEdit::positionCornerAction() {
    if (cornerAction_) {
        const int inset = qMax(UiZoom::s(6), cornerAction_->width() / 5);
        cornerAction_->move(width() - cornerAction_->width() - inset,
                            height() - cornerAction_->height() - inset);
        cornerAction_->raise();
    }
    if (leadingAction_) {
        const int horizontalInset = UiZoom::s(12);
        const int verticalInset = UiZoom::s(10);
        setViewportMargins(0, 0, 0, leadingAction_->height() + UiZoom::s(10));
        leadingAction_->move(horizontalInset,
                             height() - leadingAction_->height() - verticalInset);
        leadingAction_->raise();
        if (secondaryLeadingAction_) {
            secondaryLeadingAction_->move(leadingAction_->x() + leadingAction_->width() + UiZoom::s(8),
                                         leadingAction_->y());
            secondaryLeadingAction_->raise();
        }
        if (leadingHint_) {
            leadingHint_->move(leadingAction_->x(),
                               leadingAction_->y() - leadingHint_->height() - UiZoom::s(5));
            leadingHint_->raise();
        }
    } else {
        setViewportMargins(0, 0, 0, 0);
    }
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
