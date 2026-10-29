// 文件作用：协调实际显示、提交、取消与登录身份代次；工具只观察安全状态。
#include "app/DesktopPasswordChangeRequests.h"

#include <QApplication>
#include <QTimer>
#include <QUuid>
#include <QWidget>
#include <utility>

#include "ui/PasswordChangeDialog.h"

DesktopPasswordChangeRequests::DesktopPasswordChangeRequests(QWidget* window, QObject* parent)
    : QObject(parent), window_(window) {}

void DesktopPasswordChangeRequests::setHandlers(std::function<QString()> account,
    std::function<QString()> identity, Submit submit) {
    cancelForAccountChange();
    account_ = std::move(account);
    identity_ = std::move(identity);
    submit_ = std::move(submit);
}

bool DesktopPasswordChangeRequests::canPresent() const {
    return window_ && window_->isVisible() && qApp &&
        qApp->applicationState() == Qt::ApplicationActive && !QApplication::activeModalWidget();
}

bool DesktopPasswordChangeRequests::matchesIdentity(const State& state) const {
    return account_ && identity_ && !state.account.isEmpty() && !state.identity.isEmpty() &&
        account_() == state.account && identity_() == state.identity;
}

QJsonObject DesktopPasswordChangeRequests::result(const State& state) const {
    QJsonObject output{{QStringLiteral("request_id"), state.id},
        {QStringLiteral("opened"), state.opened}, {QStringLiteral("status"), state.status},
        {QStringLiteral("changed"), state.status == QStringLiteral("changed")}};
    if (!state.errorCode.isEmpty()) output.insert(QStringLiteral("error_code"), state.errorCode);
    return output;
}

QJsonObject DesktopPasswordChangeRequests::open() {
    if (requests_.contains(current_)) {
        State& current = requests_[current_];
        if (matchesIdentity(current) && (current.status == QStringLiteral("opening") ||
            current.status == QStringLiteral("awaiting_user") || current.status == QStringLiteral("submitting") || dialog_))
            return result(current); // 正在提交时复用真实请求，不重开表单或误标取消。
    }
    cancelForAccountChange();
    State state;
    state.id = QUuid::createUuid().toString(QUuid::WithoutBraces).toLower();
    state.account = account_ ? account_() : QString();
    state.identity = identity_ ? identity_() : QString();
    if (state.account.isEmpty() || state.identity.isEmpty()) {
        state.status = QStringLiteral("reauth_required");
        state.errorCode = QStringLiteral("reauth_required");
    } else if (!submit_ || !canPresent()) {
        state.status = QStringLiteral("failed");
        state.errorCode = QStringLiteral("unavailable");
    }
    current_ = state.id;
    requests_.insert(state.id, state);
    order_.append(state.id);
    while (order_.size() > 64) requests_.remove(order_.takeFirst());
    if (state.status == QStringLiteral("opening"))
        QTimer::singleShot(0, this, [this, id = state.id] { showRequest(id); });
    return result(state);
}

QJsonObject DesktopPasswordChangeRequests::status(const QString& id) {
    auto found = requests_.find(id);
    if (found == requests_.end()) {
        State missing;
        missing.id = id;
        missing.status = QStringLiteral("failed");
        missing.errorCode = QStringLiteral("unavailable");
        return result(missing);
    }
    if (!matchesIdentity(found.value())) {
        found->status = QStringLiteral("failed");
        found->errorCode = QStringLiteral("identity_changed");
        if (current_ == id && dialog_) {
            dialog_->dismissForIdentityChange();
            dialog_.clear();
        }
    }
    return result(found.value());
}

QJsonObject DesktopPasswordChangeRequests::request(const QJsonObject& arguments) {
    return arguments.value(QStringLiteral("action")).toString() == QStringLiteral("open") ?
        open() : status(arguments.value(QStringLiteral("request_id")).toString());
}

void DesktopPasswordChangeRequests::showRequest(const QString& id) {
    auto found = requests_.find(id);
    if (found == requests_.end() || found->status != QStringLiteral("opening")) return;
    if (!matchesIdentity(found.value()) || !canPresent()) {
        found->status = QStringLiteral("failed");
        found->errorCode = matchesIdentity(found.value()) ? QStringLiteral("unavailable") :
            QStringLiteral("identity_changed");
        return;
    }
    QPointer<DesktopPasswordChangeRequests> self(this);
    auto* dialog = new PasswordChangeDialog(found->account,
        [self, id](const QString& password, Completion completion) {
            if (!self || !self->requests_.contains(id)) { completion(QStringLiteral("unavailable")); return; }
            if (!self->matchesIdentity(self->requests_[id])) { completion(QStringLiteral("identity_changed")); return; }
            self->requests_[id].status = QStringLiteral("submitting");
            self->requests_[id].errorCode.clear();
            self->submit_(password, [self, id, completion](const QString& code) {
                if (!self || !self->requests_.contains(id)) return;
                State& state = self->requests_[id];
                const QString safeCode = self->matchesIdentity(state) ? code : QStringLiteral("identity_changed");
                state.errorCode = safeCode;
                state.status = safeCode.isEmpty() ? QStringLiteral("changed") :
                    safeCode == QStringLiteral("reauth_required") ? QStringLiteral("reauth_required") :
                    QStringLiteral("failed");
                completion(safeCode);
            });
        }, [self, id] {
            if (!self || !self->requests_.contains(id)) return;
            self->requests_[id].opened = true;
            self->requests_[id].status = QStringLiteral("awaiting_user");
        }, [self, id] {
            if (!self || !self->requests_.contains(id)) return;
            State& state = self->requests_[id];
            if (state.status != QStringLiteral("changed") && state.status != QStringLiteral("submitting")) {
                state.status = QStringLiteral("canceled");
                state.errorCode.clear();
            }
        }, window_);
    dialog_ = dialog;
    connect(dialog, &QDialog::finished, this, [this, dialog] {
        if (dialog_ == dialog) dialog_.clear();
    });
    dialog->show();
}

void DesktopPasswordChangeRequests::cancelForAccountChange() {
    if (requests_.contains(current_)) {
        State& state = requests_[current_];
        if (state.status != QStringLiteral("changed") && state.status != QStringLiteral("canceled")) {
            state.status = QStringLiteral("failed");
            state.errorCode = QStringLiteral("identity_changed");
        }
    }
    if (dialog_) { dialog_->dismissForIdentityChange(); dialog_.clear(); }
}
