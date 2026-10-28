// Agent tools reuse the same cross-device Session coordinator used by the desktop UI.
#include "MaiChatTools/DesktopRemoteSessionTools.h"

#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QPointer>
#include <QThread>
#include <QWaitCondition>

#include <memory>
#include <string>
#include <utility>

#include "MaiTool.h"
#include "agent/DesktopSessionSyncCoordinator.h"

namespace {

enum class Kind { List, Create, Delete, SetWorkspace };

std::string utf8(const QString& text) {
    const QByteArray bytes = text.toUtf8();
    return std::string(bytes.constData(), size_t(bytes.size()));
}

MaiToolResult invalid(const QString& text) {
    return MaiToolResult::failure(MaiErrorCode::InvalidInput, utf8(text));
}

struct WaitState {
    QMutex mutex;
    QWaitCondition changed;
    QJsonObject result;
    QString error;
    bool done = false;
    bool abandoned = false;

    void finish(const QJsonObject& value, const QString& failure) {
        QMutexLocker lock(&mutex);
        if (done || abandoned) return;
        result = value;
        error = failure;
        done = true;
        changed.wakeAll();
    }
};

class DesktopRemoteSessionTool final : public MaiTool {
public:
    using Provider = std::function<DesktopSessionSyncCoordinator*()>;

    DesktopRemoteSessionTool(Kind kind, QObject* dispatcher, Provider provider)
        : kind_(kind), dispatcher_(dispatcher), provider_(std::move(provider)) {}

    std::string name() const override {
        switch (kind_) {
        case Kind::List: return "maichat_remote_session_list";
        case Kind::Create: return "maichat_remote_session_create";
        case Kind::Delete: return "maichat_remote_session_delete";
        case Kind::SetWorkspace: return "maichat_remote_session_set_workspace";
        }
        return {};
    }

    std::string description() const override {
        switch (kind_) {
        case Kind::List:
            return "List Sessions and online devices belonging to this MaiChat account, excluding "
                   "this computer. Use device_slot to choose a target for remote Session operations.";
        case Kind::Create:
            return "Create a Session on another online MaiChat device. Omit workspace_path to use "
                   "that device's default Workspace. A supplied path must already exist as an "
                   "absolute path on the target device.";
        case Kind::Delete:
            return "Permanently delete a Session and its chat history on another MaiChat device. "
                   "Find its session_id with maichat_remote_session_list first. Approval "
                   "follows the selected Agent policy.";
        case Kind::SetWorkspace:
            return "Change an existing idle Session's Workspace on another online MaiChat device. "
                   "The path must already exist as an absolute directory on that device. "
                   "The Session ID and chat history stay the same. "
                   "Approval follows the selected Agent policy.";
        }
        return {};
    }

    std::string parametersSchema() const override {
        switch (kind_) {
        case Kind::List:
            return R"({"type":"object","properties":{},"additionalProperties":false})";
        case Kind::Create:
            return R"({"type":"object","properties":{"device_slot":{"type":"string"},)"
                   R"("workspace_path":{"type":"string"}},"required":["device_slot"],)"
                   R"("additionalProperties":false})";
        case Kind::Delete:
            return R"({"type":"object","properties":{"device_slot":{"type":"string"},)"
                   R"("session_id":{"type":"string"}},"required":["device_slot","session_id"],)"
                   R"("additionalProperties":false})";
        case Kind::SetWorkspace:
            return R"({"type":"object","properties":{"device_slot":{"type":"string"},)"
                   R"("session_id":{"type":"string"},"workspace_path":{"type":"string"}},)"
                   R"("required":["device_slot","session_id","workspace_path"],)"
                   R"("additionalProperties":false})";
        }
        return {};
    }

    bool requiresApproval(const std::string&) const override {
        return kind_ == Kind::Delete || kind_ == Kind::SetWorkspace;
    }
    bool requiresPerCallApproval(const std::string&) const override {
        return kind_ == Kind::Delete || kind_ == Kind::SetWorkspace;
    }

    MaiToolResult execute(const std::string& argumentsJson, const MaiToolContext& context) override {
        QJsonParseError parseError;
        const QJsonDocument parsed = QJsonDocument::fromJson(
            QByteArray(argumentsJson.data(), int(argumentsJson.size())), &parseError);
        if (parseError.error != QJsonParseError::NoError || !parsed.isObject())
            return invalid(QStringLiteral("arguments must be a JSON object"));
        if (!dispatcher_) return MaiToolResult::failure(
            MaiErrorCode::NotConfigured, "MaiChat window is closed");
        const QJsonObject arguments = parsed.object();
        if (kind_ == Kind::List) return list();
        const QString slot = arguments.value(QStringLiteral("device_slot")).toString();
        if (slot.isEmpty()) return invalid(QStringLiteral("device_slot is required"));
        QJsonObject payload;
        if (kind_ == Kind::Create) {
            payload.insert(QStringLiteral("action"), QStringLiteral("session_create"));
            if (arguments.contains(QStringLiteral("workspace_path"))) {
                if (!arguments.value(QStringLiteral("workspace_path")).isString())
                    return invalid(QStringLiteral("workspace_path must be a string"));
                const QString path = arguments.value(QStringLiteral("workspace_path")).toString();
                if (!path.isEmpty()) payload.insert(QStringLiteral("workspace_path"), path);
            }
        } else {
            const QString id = arguments.value(QStringLiteral("session_id")).toString();
            if (id.isEmpty()) return invalid(QStringLiteral("session_id is required"));
            payload.insert(QStringLiteral("action"),
                           kind_ == Kind::Delete ? QStringLiteral("session_delete") :
                                                   QStringLiteral("session_set_workspace"));
            payload.insert(QStringLiteral("target_session_id"), id);
            if (kind_ == Kind::SetWorkspace) {
                if (!arguments.value(QStringLiteral("workspace_path")).isString() ||
                    arguments.value(QStringLiteral("workspace_path")).toString().isEmpty())
                    return invalid(QStringLiteral("workspace_path is required"));
                payload.insert(QStringLiteral("workspace_path"),
                               arguments.value(QStringLiteral("workspace_path")).toString());
            }
        }
        return mutate(slot, payload, context);
    }

private:
    MaiToolResult list() const {
        QJsonArray devices;
        QJsonArray sessions;
        QString error;
        const auto read = [&] {
            auto* coordinator = provider_ ? provider_() : nullptr;
            if (!coordinator) {
                error = QStringLiteral("设备同步服务尚未连接");
                return;
            }
            devices = coordinator->remoteDevices();
            sessions = coordinator->remoteSessions();
        };
        if (QThread::currentThread() == dispatcher_->thread()) read();
        else if (!QMetaObject::invokeMethod(dispatcher_.data(), read, Qt::BlockingQueuedConnection))
            return MaiToolResult::failure(MaiErrorCode::Internal,
                                          "could not read MaiChat device catalog");
        if (!error.isEmpty())
            return MaiToolResult::failure(MaiErrorCode::NotConfigured, utf8(error));
        QJsonArray deviceItems;
        for (const QJsonValue& value : devices) {
            const QJsonObject device = value.toObject();
            deviceItems.append(QJsonObject{
                {QStringLiteral("device_slot"), device.value(QStringLiteral("device_slot"))},
                {QStringLiteral("device_name"), device.value(QStringLiteral("device_name"))},
                {QStringLiteral("online"), device.value(QStringLiteral("online"))}});
        }
        QJsonArray sessionItems;
        constexpr int limit = 100;
        for (const QJsonValue& value : sessions) {
            if (sessionItems.size() >= limit) break;
            const QJsonObject session = value.toObject();
            sessionItems.append(QJsonObject{
                {QStringLiteral("device_slot"), session.value(QStringLiteral("device_slot"))},
                {QStringLiteral("session_id"), session.value(QStringLiteral("id"))},
                {QStringLiteral("title"), session.value(QStringLiteral("title"))},
                {QStringLiteral("busy"), session.value(QStringLiteral("busy"))},
                {QStringLiteral("online"), session.value(QStringLiteral("online"))}});
        }
        const QJsonObject output{{QStringLiteral("devices"), deviceItems},
                                 {QStringLiteral("sessions"), sessionItems},
                                 {QStringLiteral("total_sessions"), sessions.size()}};
        return MaiToolResult::success(
            QJsonDocument(output).toJson(QJsonDocument::Compact).toStdString(),
            sessions.size() > limit);
    }

    MaiToolResult mutate(const QString& slot, const QJsonObject& payload,
                         const MaiToolContext& context) const {
        if (context.isCanceled())
            return MaiToolResult::failure(MaiErrorCode::Canceled, "Session operation was canceled");
        if (QThread::currentThread() == dispatcher_->thread())
            return MaiToolResult::failure(MaiErrorCode::Internal,
                                          "remote Session tool requires a worker thread");
        auto state = std::make_shared<WaitState>();
        const Provider provider = provider_;
        const bool invoked = QMetaObject::invokeMethod(
            dispatcher_.data(), [state, provider, slot, payload] {
                {
                    QMutexLocker lock(&state->mutex);
                    if (state->abandoned) return;
                }
                auto* coordinator = provider ? provider() : nullptr;
                if (!coordinator) {
                    state->finish({}, QStringLiteral("设备同步服务尚未连接"));
                    return;
                }
                QJsonObject target;
                for (const QJsonValue& value : coordinator->remoteDevices()) {
                    const QJsonObject device = value.toObject();
                    if (device.value(QStringLiteral("device_slot")).toString() == slot &&
                        device.value(QStringLiteral("online")).toBool()) {
                        target = device;
                        break;
                    }
                }
                if (target.isEmpty()) {
                    state->finish({}, QStringLiteral("所属设备离线或不存在"));
                    return;
                }
                if (payload.value(QStringLiteral("action")) != QStringLiteral("session_create")) {
                    const QString id = payload.value(QStringLiteral("target_session_id")).toString();
                    bool found = false;
                    for (const QJsonValue& value : coordinator->remoteSessions()) {
                        const QJsonObject session = value.toObject();
                        if (session.value(QStringLiteral("id")).toString() != id ||
                            session.value(QStringLiteral("device_slot")).toString() != slot ||
                            session.value(QStringLiteral("login_session_id")) !=
                                target.value(QStringLiteral("login_session_id"))) continue;
                        if (session.value(QStringLiteral("busy")).toBool()) {
                            state->finish({}, QStringLiteral("对话仍在执行，请先停止"));
                            return;
                        }
                        found = true;
                        break;
                    }
                    if (!found) {
                        state->finish({}, QStringLiteral("所属设备没有这个对话"));
                        return;
                    }
                }
                coordinator->sendRemoteLifecycle(
                    target, payload,
                    [state](const QJsonObject& result, const QString& error) {
                        state->finish(result, error);
                    });
            }, Qt::QueuedConnection);
        if (!invoked)
            return MaiToolResult::failure(MaiErrorCode::Internal,
                                          "could not dispatch remote Session operation");
        QElapsedTimer timer;
        timer.start();
        QMutexLocker lock(&state->mutex);
        while (!state->done && timer.elapsed() < 35'000 && !context.isCanceled())
            state->changed.wait(&state->mutex, 100);
        if (context.isCanceled()) {
            state->abandoned = true;
            return MaiToolResult::failure(MaiErrorCode::Canceled, "Session operation was canceled");
        }
        if (!state->done) {
            state->abandoned = true;
            return MaiToolResult::failure(MaiErrorCode::Network,
                                          "Session operation status is unknown; inspect the target device");
        }
        if (!state->error.isEmpty()) {
            const MaiErrorCode code = state->result.contains(QStringLiteral("ok")) ?
                MaiErrorCode::InvalidInput : MaiErrorCode::Network;
            return MaiToolResult::failure(code, utf8(state->error));
        }
        if (!state->result.value(QStringLiteral("ok")).toBool())
            return MaiToolResult::failure(MaiErrorCode::Protocol,
                                          "target device returned no Session result");
        QJsonObject output{{QStringLiteral("ok"), true},
                           {QStringLiteral("device_slot"), slot},
                           {QStringLiteral("session_id"),
                            state->result.value(QStringLiteral("session_id"))}};
        if (kind_ == Kind::SetWorkspace)
            output.insert(QStringLiteral("workspace_path"),
                          state->result.value(QStringLiteral("workspace_path"))
                              .toString(payload.value(QStringLiteral("workspace_path")).toString()));
        return MaiToolResult::success(
            QJsonDocument(output).toJson(QJsonDocument::Compact).toStdString());
    }

    Kind kind_;
    QPointer<QObject> dispatcher_;
    Provider provider_;
};

}  // namespace

void registerDesktopRemoteSessionTools(
    MaiToolRegistry& registry, QObject* dispatcher,
    std::function<DesktopSessionSyncCoordinator*()> coordinatorProvider) {
    registry.add(std::make_unique<DesktopRemoteSessionTool>(
        Kind::List, dispatcher, coordinatorProvider));
    registry.add(std::make_unique<DesktopRemoteSessionTool>(
        Kind::Create, dispatcher, coordinatorProvider));
    registry.add(std::make_unique<DesktopRemoteSessionTool>(
        Kind::Delete, dispatcher, coordinatorProvider));
    registry.add(std::make_unique<DesktopRemoteSessionTool>(
        Kind::SetWorkspace, dispatcher, std::move(coordinatorProvider)));
}
