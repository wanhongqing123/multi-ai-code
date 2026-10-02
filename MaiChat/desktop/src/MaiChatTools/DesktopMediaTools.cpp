#include "MaiChatTools/DesktopMediaTools.h"

#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QPointer>
#include <QThread>
#include <QWidget>

#include <memory>
#include <string>

#include "MaiTool.h"
#include "ui/MaiFfplayVideoDialog.h"

namespace {

MaiToolResult invalid(const char* reason) {
    return MaiToolResult::failure(MaiErrorCode::InvalidInput, reason);
}

QJsonObject parseArguments(const std::string& value) {
    return QJsonDocument::fromJson(QByteArray(value.data(), static_cast<int>(value.size()))).object();
}

class DesktopPlayVideoTool final : public MaiTool {
public:
    explicit DesktopPlayVideoTool(QWidget* window) : window_(window) {}
    std::string name() const override { return "maichat_play_video"; }
    std::string description() const override {
        return "Open a local video in MaiChat's FFplay/Graphics popup. The Agent and IM "
               "message video use the same player. Relative paths start in the Agent workspace.";
    }
    std::string parametersSchema() const override {
        return R"({"type":"object","properties":{"path":{"type":"string"}},"required":["path"],"additionalProperties":false})";
    }
    MaiToolResult execute(const std::string& argumentsJson,
                          const MaiToolContext& context) override {
        const QJsonObject arguments = parseArguments(argumentsJson);
        const QString requested = arguments.value(QStringLiteral("path")).toString().trimmed();
        if (requested.isEmpty()) return invalid("path is required");
        const QByteArray bytes = requested.toUtf8();
        const std::string resolved = context.resolvePath(
            std::string(bytes.constData(), static_cast<size_t>(bytes.size())));
        if (resolved.empty()) return invalid("video path is outside the accessible area");
        const QString path = QString::fromUtf8(resolved.data(), static_cast<int>(resolved.size()));
        if (!QFileInfo(path).isFile())
            return MaiToolResult::failure(MaiErrorCode::NotFound, "video file does not exist");
        if (window_.isNull())
            return MaiToolResult::failure(MaiErrorCode::NotConfigured, "MaiChat window is closed");
        bool opened = false;
        const auto show = [&] {
            if (window_.isNull()) return;
            if (auto existing = MaiFfplayVideoDialog::activeDialog()) existing->close();
            auto* dialog = new MaiFfplayVideoDialog(path, window_.data());
            dialog->setAttribute(Qt::WA_DeleteOnClose);
            dialog->show();
            opened = dialog->isStarted();
            if (!opened) dialog->close();
        };
        if (QThread::currentThread() == window_->thread()) show();
        else if (!QMetaObject::invokeMethod(window_.data(), show, Qt::BlockingQueuedConnection))
            return MaiToolResult::failure(MaiErrorCode::Internal,
                                          "cannot dispatch video playback to MaiChat UI");
        if (!opened)
            return MaiToolResult::failure(MaiErrorCode::Internal,
                                          "FFplay or Graphics could not open the video popup");
        return MaiToolResult::success(R"({"opened":true})");
    }

private:
    QPointer<QWidget> window_;
};

class DesktopVideoCommandTool final : public MaiTool {
public:
    explicit DesktopVideoCommandTool(QWidget* window) : window_(window) {}
    std::string name() const override { return "maichat_video_command"; }
    std::string description() const override {
        return "Control the currently open FFplay video popup. Supported actions include "
               "play, pause, step, relative or chapter seek, percent seek, stream/filter cycling, mute, volume, "
               "fullscreen, and close.";
    }
    std::string parametersSchema() const override {
        return R"({"type":"object","properties":{"action":{"type":"string","enum":["play","pause","toggle_pause","step","seek_forward","seek_backward","seek_minute_forward","seek_minute_backward","next_chapter","previous_chapter","seek_percent","next_audio","next_video","next_subtitle","next_program","next_filter","mute","volume_up","volume_down","fullscreen","close"]},"percent":{"type":"number","minimum":0,"maximum":100}},"required":["action"],"additionalProperties":false})";
    }
    MaiToolResult execute(const std::string& argumentsJson,
                          const MaiToolContext&) override {
        const QJsonObject arguments = parseArguments(argumentsJson);
        const QString action = arguments.value(QStringLiteral("action")).toString();
        if (action.isEmpty()) return invalid("action is required");
        if (window_.isNull())
            return MaiToolResult::failure(MaiErrorCode::NotConfigured, "MaiChat window is closed");
        bool found = false;
        bool accepted = false;
        const auto send = [&] {
            auto dialog = MaiFfplayVideoDialog::activeDialog();
            if (dialog.isNull()) return;
            found = true;
            if (action == QStringLiteral("seek_percent")) {
                const auto value = arguments.value(QStringLiteral("percent"));
                if (value.isDouble() && value.toDouble() >= 0 && value.toDouble() <= 100)
                    accepted = dialog->seekPercent(value.toDouble() / 100.0);
            } else if (action == QStringLiteral("close")) {
                dialog->close();
                accepted = true;
            } else if (action == QStringLiteral("play")) {
                accepted = dialog->isPlaying() || dialog->sendCommand(QStringLiteral("pause"));
            } else if (action == QStringLiteral("pause")) {
                accepted = !dialog->isPlaying() || dialog->sendCommand(QStringLiteral("pause"));
            } else if (action == QStringLiteral("toggle_pause")) {
                accepted = dialog->sendCommand(QStringLiteral("pause"));
            } else {
                accepted = dialog->sendCommand(action);
            }
        };
        if (QThread::currentThread() == window_->thread()) send();
        else if (!QMetaObject::invokeMethod(window_.data(), send, Qt::BlockingQueuedConnection))
            return MaiToolResult::failure(MaiErrorCode::Internal,
                                          "cannot dispatch video command to MaiChat UI");
        if (!found)
            return MaiToolResult::failure(MaiErrorCode::NotFound, "no MaiChat video popup is open");
        if (!accepted) return invalid("invalid or unavailable video command");
        return MaiToolResult::success(R"({"accepted":true})");
    }

private:
    QPointer<QWidget> window_;
};

}  // namespace

void registerDesktopMediaTools(MaiToolRegistry& registry, QWidget* window) {
    registry.add(std::make_unique<DesktopPlayVideoTool>(window));
    registry.add(std::make_unique<DesktopVideoCommandTool>(window));
}
