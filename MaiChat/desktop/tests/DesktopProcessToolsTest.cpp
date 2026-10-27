#include <QCoreApplication>
#include <QTemporaryDir>
#include <QThread>

#include <json.hpp>

#include "MaiChatTools/DesktopProcessTools.h"
#include "MaiTool.h"

#include <cstdio>
#include <memory>
#ifndef Q_OS_WIN
#include <cerrno>
#include <csignal>
#include <cstdlib>
#endif

namespace {

#define CHECK(value) do { if (!(value)) { \
    std::fprintf(stderr, "check failed: %s:%d: %s\n", __FILE__, __LINE__, #value); \
    return 1; } } while (false)

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir workspace;
    CHECK(workspace.isValid());
    const auto manager = std::make_shared<DesktopProcessManager>();
    MaiToolRegistry registry;
    registerDesktopProcessTools(registry, manager);
    auto* startTool = registry.find("desktop_process_start");
    auto* readTool = registry.find("desktop_process_read");
    auto* writeTool = registry.find("desktop_process_write");
    auto* stopTool = registry.find("desktop_process_stop");
    CHECK(startTool && readTool && writeTool && stopTool);
    for (MaiTool* tool : {startTool, readTool, writeTool, stopTool})
        CHECK(nlohmann::json::parse(tool->parametersSchema()).is_object());
    CHECK(startTool->requiresPerCallApproval("{}"));
    CHECK(writeTool->requiresPerCallApproval("{}"));
    CHECK(stopTool->requiresPerCallApproval("{}"));
    CHECK(!readTool->requiresApproval("{}"));

    MaiToolContext owner;
    owner.sessionId = "session-owner";
    owner.root = workspace.path().toUtf8().toStdString();
#ifdef Q_OS_WIN
    const char* interactive = "echo ready & set /p line= & echo got:%line%";
#else
    const char* interactive = "printf 'ready\\n'; read line; printf 'got:%s\\n' \"$line\"";
#endif
    const auto started = startTool->execute(
        nlohmann::json{{"command", interactive}}.dump(), owner);
    CHECK(!started.hasError());
    const auto id = nlohmann::json::parse(started.output())["process_id"].get<std::string>();
    MaiToolContext other = owner;
    other.sessionId = "session-other";
    CHECK(manager->read(other.sessionId, id, 0).error().code() == MaiErrorCode::NotFound);
    CHECK(manager->write(other.sessionId, id, "bad\n").code() == MaiErrorCode::NotFound);

    std::uint64_t cursor = 0;
    bool sawReady = false;
    for (int attempt = 0; attempt < 50; ++attempt) {
        const auto view = manager->read(owner.sessionId, id, cursor);
        CHECK(view);
        cursor = view.value().cursor;
        if (view.value().output.find("ready") != std::string::npos) {
            sawReady = true;
            break;
        }
        QThread::msleep(20);
    }
    CHECK(sawReady);
    CHECK(!writeTool->execute(nlohmann::json{{"process_id", id},
                                                   {"input", "hello\n"}}.dump(), owner).hasError());
    bool sawReply = false;
    for (int attempt = 0; attempt < 80; ++attempt) {
        const auto view = manager->read(owner.sessionId, id, cursor);
        CHECK(view);
        cursor = view.value().cursor;
        if (view.value().output.find("got:hello") != std::string::npos) {
            sawReply = true;
            break;
        }
        QThread::msleep(20);
    }
#ifndef Q_OS_WIN
    CHECK(sawReply);
#endif
    CHECK(manager->read(owner.sessionId, id, cursor + 1000000).error().code() ==
          MaiErrorCode::InvalidInput);

#ifdef Q_OS_WIN
    const char* longCommand = "ping -n 30 127.0.0.1 > nul";
#else
    const char* longCommand = "sleep 30";
#endif
    const auto waiting = manager->start(owner.sessionId, owner.root, longCommand);
    CHECK(waiting);
    CHECK(!stopTool->execute(nlohmann::json{{"process_id", waiting.value()}}.dump(),
                             owner).hasError());
    const auto stopped = manager->read(owner.sessionId, waiting.value(), 0);
    CHECK(stopped && !stopped.value().running);
#ifndef Q_OS_WIN
    const auto tree = manager->start(owner.sessionId, owner.root,
        "sleep 30 & child=$!; printf 'child:%s\\n' \"$child\"; wait");
    CHECK(tree);
    int child = 0;
    for (int attempt = 0; attempt < 50; ++attempt) {
        const auto output = manager->read(owner.sessionId, tree.value(), 0);
        CHECK(output);
        const auto marker = output.value().output.find("child:");
        if (marker != std::string::npos) {
            child = std::atoi(output.value().output.c_str() + marker + 6);
            break;
        }
        QThread::msleep(20);
    }
    CHECK(child > 0);
    CHECK(!manager->stop(owner.sessionId, tree.value()));
    bool childGone = false;
    for (int attempt = 0; attempt < 40; ++attempt) {
        if (::kill(child, 0) != 0 && errno == ESRCH) {
            childGone = true;
            break;
        }
        QThread::msleep(50);
    }
    CHECK(childGone);
#endif
    return 0;
}
