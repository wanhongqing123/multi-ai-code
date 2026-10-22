// 文件作用：桌面端 Agent 工具；DesktopMediaTools 负责 Agent 工具注册、调用与结果返回，本文件给出其实现。
// 直接依赖：MaiTool.h、ui/MaiFfplayVideoDialog.h；实现与这些接口协作。
#include "MaiChatTools/DesktopMediaTools.h"

#include <QFileInfo>
#include <QFileDialog>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QMetaObject>
#include <QPointer>
#include <QStandardPaths>
#include <QThread>
#include <QWidget>

#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>

#include "MaiTool.h"
#include "ui/DesktopCameraDialog.h"
#include "ui/DesktopMusicPlayerDialog.h"
#include "ui/MaiFfplayVideoDialog.h"
#include "ui/MainWindow.h"

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
        return "Open a local video in MaiChat's FFplay/Graphics player at optional "
               "start_seconds of source-media time. The Agent and IM message video use the "
               "same player. Relative paths start in the Agent workspace.";
    }
    std::string parametersSchema() const override {
        return R"({"type":"object","properties":{"path":{"type":"string"},)"
            R"("start_seconds":{"type":"number","minimum":0}},"required":["path"],)"
            R"("additionalProperties":false})";
    }
    MaiToolResult execute(const std::string& argumentsJson,
                          const MaiToolContext& context) override {
        const QJsonObject arguments = parseArguments(argumentsJson);
        const QString requested = arguments.value(QStringLiteral("path")).toString().trimmed();
        if (requested.isEmpty()) return invalid("path is required");
        double startSeconds = 0;
        if (arguments.contains(QStringLiteral("start_seconds"))) {
            const QJsonValue value = arguments.value(QStringLiteral("start_seconds"));
            if (!value.isDouble()) return invalid("start_seconds must be a nonnegative source-media time");
            startSeconds = value.toDouble();
            if (!std::isfinite(startSeconds) || startSeconds < 0 ||
                startSeconds > static_cast<double>(std::numeric_limits<int64_t>::max()) / 1'000'000)
                return invalid("start_seconds must be a nonnegative source-media time");
        }
        const int64_t startPositionUs = static_cast<int64_t>(startSeconds * 1'000'000);
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
            if (auto* mainWindow = dynamic_cast<MainWindow*>(window_.data())) {
                opened = mainWindow->openDockedVideo(path, {}, startPositionUs);
                return;
            }
            if (auto existing = MaiFfplayVideoDialog::activeDialog()) existing->close();
            auto* dialog = new MaiFfplayVideoDialog(path, window_.data(), startPositionUs);
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
                                          "FFplay or Graphics could not open the video player");
        return MaiToolResult::success(
            QJsonDocument(QJsonObject{{QStringLiteral("opened"), true},
                                      {QStringLiteral("start_seconds"), startSeconds}})
                .toJson(QJsonDocument::Compact).toStdString());
    }

private:
    QPointer<QWidget> window_;
};

class DesktopTakePhotoTool final : public MaiTool {
public:
    explicit DesktopTakePhotoTool(QWidget* window) : window_(window) {}
    std::string name() const override { return "maichat_take_photo"; }
    std::string description() const override {
        return "Open the desktop camera preview and wait for the user to press the shutter. "
               "Save a new JPEG in the Agent workspace; cancel creates no photo.";
    }
    std::string parametersSchema() const override {
        return R"({"type":"object","properties":{},"additionalProperties":false})";
    }
    bool requiresApproval(const std::string&) const override { return true; }
    bool requiresPerCallApproval(const std::string&) const override { return true; }
    MaiToolResult execute(const std::string&, const MaiToolContext& context) override {
        if (window_.isNull())
            return MaiToolResult::failure(MaiErrorCode::NotConfigured,
                                          "MaiChat window is closed");
        const std::string resolved = context.resolvePath(".");
        if (resolved.empty())
            return invalid("an accessible Agent workspace is required");
        const QString directory = QString::fromUtf8(resolved.data(), static_cast<int>(resolved.size()));
        QString photo;
        QString error;
        const auto capture = [&] {
            if (!window_.isNull())
                photo = DesktopCameraDialog::capturePhoto(window_.data(), directory, &error);
        };
        if (QThread::currentThread() == window_->thread()) capture();
        else if (!QMetaObject::invokeMethod(window_.data(), capture, Qt::BlockingQueuedConnection))
            return MaiToolResult::failure(MaiErrorCode::Internal,
                                          "cannot dispatch camera to MaiChat UI");
        if (!error.isEmpty())
            return MaiToolResult::failure(MaiErrorCode::Internal, error.toStdString());
        if (photo.isEmpty())
            return MaiToolResult::failure(MaiErrorCode::Canceled, "photo capture was canceled");
        const QString relative = QDir(directory).relativeFilePath(photo);
        return MaiToolResult::success(
            QJsonDocument(QJsonObject{{QStringLiteral("path"), relative},
                                      {QStringLiteral("mime_type"), QStringLiteral("image/jpeg")}})
                .toJson(QJsonDocument::Compact).toStdString());
    }
private:
    QPointer<QWidget> window_;
};

class DesktopPlayLocalMusicTool final : public MaiTool {
public:
    explicit DesktopPlayLocalMusicTool(QWidget* window) : window_(window) {}
    std::string name() const override { return "maichat_play_local_music"; }
    std::string description() const override {
        return "Open an in-app player for a local MP3, M4A, or WAV. Provide a path in the "
               "Agent workspace, or omit path to let the user choose a file. Never scan the "
               "user's Music directory without the file picker.";
    }
    std::string parametersSchema() const override {
        return R"({"type":"object","properties":{"path":{"type":"string"}},)"
            R"("additionalProperties":false})";
    }
    bool requiresApproval(const std::string&) const override { return true; }
    bool requiresPerCallApproval(const std::string&) const override { return true; }
    MaiToolResult execute(const std::string& argumentsJson,
                          const MaiToolContext& context) override {
        if (window_.isNull())
            return MaiToolResult::failure(MaiErrorCode::NotConfigured,
                                          "MaiChat window is closed");
        const QJsonObject arguments = parseArguments(argumentsJson);
        QString chosen;
        if (arguments.contains(QStringLiteral("path"))) {
            const QJsonValue value = arguments.value(QStringLiteral("path"));
            if (!value.isString() || value.toString().trimmed().isEmpty())
                return invalid("path must be a local audio file");
            const QByteArray bytes = value.toString().trimmed().toUtf8();
            const std::string resolved = context.resolvePath(
                std::string(bytes.constData(), static_cast<size_t>(bytes.size())));
            if (resolved.empty()) return invalid("audio path is outside the accessible workspace");
            chosen = QString::fromUtf8(resolved.data(), static_cast<int>(resolved.size()));
        }
        QString error;
        bool opened = false;
        const auto show = [&] {
            if (window_.isNull()) return;
            if (chosen.isEmpty())
                chosen = QFileDialog::getOpenFileName(window_.data(), QStringLiteral("选择本地音乐"),
                    QStandardPaths::writableLocation(QStandardPaths::MusicLocation),
                    QStringLiteral("音频文件 (*.mp3 *.m4a *.wav)"));
            if (!chosen.isEmpty())
                opened = DesktopMusicPlayerDialog::openFile(window_.data(), chosen, &error);
        };
        if (QThread::currentThread() == window_->thread()) show();
        else if (!QMetaObject::invokeMethod(window_.data(), show, Qt::BlockingQueuedConnection))
            return MaiToolResult::failure(MaiErrorCode::Internal,
                                          "cannot dispatch music player to MaiChat UI");
        if (!error.isEmpty())
            return MaiToolResult::failure(MaiErrorCode::InvalidInput, error.toStdString());
        if (!opened)
            return MaiToolResult::failure(MaiErrorCode::Canceled, "music selection was canceled");
        return MaiToolResult::success(
            QJsonDocument(QJsonObject{{QStringLiteral("opened"), true},
                                      {QStringLiteral("file_name"), QFileInfo(chosen).fileName()}})
                .toJson(QJsonDocument::Compact).toStdString());
    }
private:
    QPointer<QWidget> window_;
};

class DesktopVideoCommandTool final : public MaiTool {
public:
    explicit DesktopVideoCommandTool(QWidget* window) : window_(window) {}
    std::string name() const override { return "maichat_video_command"; }
    std::string description() const override {
        // set_speed 在当前弹窗内重启同一 FFplay 源文件并回到原片位置；
        // atempo 保持音调，音画同步，不生成转码文件，但切速有短暂中断。
        return "Control the currently open FFplay video player. set_speed accepts speed "
               "0.25, 0.5, 1, 1.5, or 2; it keeps audio in sync and preserves pitch through "
               "FFmpeg atempo. Changing speed briefly restarts the same popup at its source "
               "position without creating a new media file. seek_seconds jumps to an absolute "
               "source-media time; seek_percent and the existing play, pause, step, relative "
               "or chapter seek, replay, previous_item, next_item, stream/filter cycling, "
               "mute, volume, fullscreen, and close "
               "actions remain available.";
    }
    std::string parametersSchema() const override {
        return R"({"type":"object","properties":{"action":{"type":"string","enum":["play","pause","toggle_pause",)"
            R"("step","replay","previous_item","next_item","seek_forward","seek_backward",)"
            R"("seek_minute_forward","seek_minute_backward","next_chapter",)"
            R"("previous_chapter","seek_percent","seek_seconds","set_speed","next_audio","next_video","next_subtitle",)"
            R"("next_program","next_filter","mute","volume_up","volume_down","fullscreen","close"]},)"
            R"("percent":{"type":"number","minimum":0,"maximum":100},"seconds":{"type":"number","minimum":0},)"
            R"("speed":{"type":"number","enum":[0.25,0.5,1,1.5,2]}},"required":["action"],"additionalProperties":false)"
            R"(})";
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
            } else if (action == QStringLiteral("seek_seconds")) {
                const auto value = arguments.value(QStringLiteral("seconds"));
                if (value.isDouble()) accepted = dialog->seekSeconds(value.toDouble());
            } else if (action == QStringLiteral("set_speed")) {
                const auto value = arguments.value(QStringLiteral("speed"));
                if (value.isDouble()) accepted = dialog->setSpeed(value.toDouble());
            } else if (action == QStringLiteral("replay")) {
                accepted = dialog->replay();
            } else if (action == QStringLiteral("previous_item")) {
                accepted = dialog->navigateVideo(-1);
            } else if (action == QStringLiteral("next_item")) {
                accepted = dialog->navigateVideo(1);
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
    registry.add(std::make_unique<DesktopTakePhotoTool>(window));
    registry.add(std::make_unique<DesktopPlayLocalMusicTool>(window));
    registry.add(std::make_unique<DesktopVideoCommandTool>(window));
}
