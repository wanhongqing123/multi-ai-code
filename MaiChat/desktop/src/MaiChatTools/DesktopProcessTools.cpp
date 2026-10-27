#include "MaiChatTools/DesktopProcessTools.h"

#include <QByteArray>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QString>
#include <QUuid>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <initializer_list>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#ifndef Q_OS_WIN
#include <signal.h>
#include <unistd.h>
#else
#include <windows.h>
#endif

#include <json.hpp>

#include "MaiTool.h"

namespace {

using Json = nlohmann::json;
constexpr std::size_t kRetainedOutputBytes = 1024 * 1024;
constexpr std::size_t kReadBytes = 32 * 1024;
constexpr std::size_t kMaxSessions = 12;

#ifndef Q_OS_WIN
class GroupedProcess final : public QProcess {
protected:
    void setupChildProcess() override { ::setpgid(0, 0); }
};
#else
using GroupedProcess = QProcess;
#endif

struct ProcessSession {
    std::string id;
    std::string owner;
    std::uint64_t createdOrder = 0;
    QString workingDirectory;
    QString command;
    mutable std::mutex mutex;
    std::condition_variable changed;
    std::thread worker;
    std::deque<QByteArray> pendingInput;
    std::size_t pendingBytes = 0;
    QByteArray output;
    std::uint64_t firstCursor = 0;
    bool ready = false;
    bool started = false;
    bool running = false;
    bool stopRequested = false;
    int exitCode = 0;
    std::string startError;
};

void appendOutput(ProcessSession& session, const QByteArray& bytes) {
    if (bytes.isEmpty()) return;
    std::lock_guard<std::mutex> lock(session.mutex);
    session.output.append(bytes);
    if (session.output.size() > int(kRetainedOutputBytes)) {
        const int discarded = session.output.size() - int(kRetainedOutputBytes);
        session.output.remove(0, discarded);
        session.firstCursor += std::uint64_t(discarded);
    }
    session.changed.notify_all();
}

void runProcess(const std::shared_ptr<ProcessSession>& session) {
    GroupedProcess process;
    process.setWorkingDirectory(session->workingDirectory);
    process.setProcessChannelMode(QProcess::MergedChannels);
#ifdef Q_OS_WIN
    process.setProgram(QStringLiteral("cmd.exe"));
    process.setArguments({QStringLiteral("/D"), QStringLiteral("/S"),
                          QStringLiteral("/C"), session->command});
#else
    process.setProgram(QStringLiteral("/bin/sh"));
    process.setArguments({QStringLiteral("-c"), session->command});
#endif
    process.start();
    const bool started = process.waitForStarted(5000);
#ifndef Q_OS_WIN
    const pid_t groupId = started ? pid_t(process.processId()) : 0;
#else
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    bool ownsProcessTree = false;
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                    &limits, sizeof(limits)) && started) {
            HANDLE child = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE,
                                       FALSE, DWORD(process.processId()));
            if (child) {
                ownsProcessTree = AssignProcessToJobObject(job, child) != FALSE;
                CloseHandle(child);
            }
        }
    }
#endif
    {
        std::lock_guard<std::mutex> lock(session->mutex);
        session->ready = true;
        session->started = started;
        session->running = started;
        if (!started) session->startError = process.errorString().toUtf8().toStdString();
        session->changed.notify_all();
    }
    if (!started) {
#ifdef Q_OS_WIN
        if (job) CloseHandle(job);
#endif
        return;
    }

    while (true) {
        process.waitForReadyRead(50);
        appendOutput(*session, process.readAllStandardOutput());
        std::deque<QByteArray> input;
        bool stop = false;
        {
            std::lock_guard<std::mutex> lock(session->mutex);
            if (process.bytesToWrite() < 64 * 1024) {
                input.swap(session->pendingInput);
                session->pendingBytes = 0;
            }
            stop = session->stopRequested;
        }
        if (stop) {
#ifndef Q_OS_WIN
            if (groupId > 0) ::kill(-groupId, SIGTERM);
#else
            if (ownsProcessTree) TerminateJobObject(job, 1);
            else process.terminate();
#endif
            if (!process.waitForFinished(1000)) {
#ifndef Q_OS_WIN
                if (groupId > 0) ::kill(-groupId, SIGKILL);
#endif
                process.kill();
                process.waitForFinished(1000);
            }
        } else {
            for (const QByteArray& bytes : input) process.write(bytes);
            if (!input.empty()) process.waitForBytesWritten(500);
        }
        process.waitForFinished(0);
        if (process.state() == QProcess::NotRunning) break;
    }
#ifndef Q_OS_WIN
    // A shell may exit while a background child remains in its process group.
    if (groupId > 0) ::kill(-groupId, SIGKILL);
#else
    if (job) CloseHandle(job);
#endif
    appendOutput(*session, process.readAllStandardOutput());
    {
        std::lock_guard<std::mutex> lock(session->mutex);
        session->exitCode = process.exitStatus() == QProcess::NormalExit ?
            process.exitCode() : -1;
        session->running = false;
        session->changed.notify_all();
    }
}

std::shared_ptr<ProcessSession> ownedSession(
    const std::map<std::string, std::shared_ptr<ProcessSession>>& sessions,
    const std::string& owner, const std::string& id) {
    const auto found = sessions.find(id);
    if (found == sessions.end() || found->second->owner != owner) return nullptr;
    return found->second;
}

MaiToolResult failure(const MaiError& error) {
    return MaiToolResult::failure(error.code(), error.message());
}

class DesktopProcessTool final : public MaiTool {
public:
    enum class Action { Start, Read, Write, Stop };
    DesktopProcessTool(Action action, std::shared_ptr<DesktopProcessManager> manager)
        : action_(action), manager_(std::move(manager)) {}

    std::string name() const override {
        switch (action_) {
        case Action::Start: return "desktop_process_start";
        case Action::Read: return "desktop_process_read";
        case Action::Write: return "desktop_process_write";
        case Action::Stop: return "desktop_process_stop";
        }
        return {};
    }

    std::string description() const override {
        switch (action_) {
        case Action::Start:
            return "Start one interactive or long-running shell command in this Desktop "
                   "Session's working directory and "
                   "return a process_id. The command can continue across Agent turns. It runs "
                   "with the App user's OS permissions. Every start requires approval. "
                   "Prefer shell for one-shot commands; use read for "
                   "incremental output and stop when finished.";
        case Action::Read:
            return "Read bounded new output from a process started in this Session. Pass the "
                   "returned cursor on the next call. The oldest output may be discarded after "
                   "1 MiB; the response says when that happened.";
        case Action::Write:
            return "Send text to stdin of a running process owned by this Session. This queues "
                   "input, so read again to confirm the process handled it. Every write needs approval.";
        case Action::Stop:
            return "Terminate a running process owned by this Session. Every stop needs approval; "
                   "read afterward for its final output and exit status.";
        }
        return {};
    }

    std::string parametersSchema() const override {
        switch (action_) {
        case Action::Start:
            return R"({"type":"object","properties":{)"
                   R"("command":{"type":"string","minLength":1,"maxLength":8192}},)"
                   R"("required":["command"],"additionalProperties":false})";
        case Action::Read:
            return R"({"type":"object","properties":{)"
                   R"("process_id":{"type":"string"},)"
                   R"("cursor":{"type":"integer","minimum":0}},)"
                   R"("required":["process_id"],"additionalProperties":false})";
        case Action::Write:
            return R"({"type":"object","properties":{)"
                   R"("process_id":{"type":"string"},)"
                   R"("input":{"type":"string","minLength":1,"maxLength":16384}},)"
                   R"("required":["process_id","input"],"additionalProperties":false})";
        case Action::Stop:
            return R"({"type":"object","properties":{"process_id":{"type":"string"}},)"
                   R"("required":["process_id"],"additionalProperties":false})";
        }
        return {};
    }

    bool requiresApproval(const std::string&) const override {
        return action_ != Action::Read;
    }
    bool requiresPerCallApproval(const std::string&) const override {
        return action_ != Action::Read;
    }

    MaiToolResult execute(const std::string& raw, const MaiToolContext& context) override {
        if (context.isCanceled())
            return MaiToolResult::failure(MaiErrorCode::Canceled, "process operation canceled");
        if (context.sessionId.empty())
            return MaiToolResult::failure(MaiErrorCode::NotConfigured,
                                          "a Desktop Session is required");
        const Json args = Json::parse(raw, nullptr, false);
        if (!args.is_object())
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "process arguments must be an object");
        auto exact = [&args](std::initializer_list<const char*> allowed) {
            for (auto item = args.begin(); item != args.end(); ++item) {
                bool found = false;
                for (const char* field : allowed) found |= item.key() == field;
                if (!found) return false;
            }
            return true;
        };
        if (action_ == Action::Start) {
            if (!exact({"command"}) || !args.value("command", Json{}).is_string())
                return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                              "command is required");
            const auto started = manager_->start(context.sessionId, context.root,
                                                  args["command"].get<std::string>());
            if (!started) return failure(started.error());
            return MaiToolResult::success(Json{{"process_id", started.value()},
                                               {"cursor", 0}, {"started", true}}.dump());
        }
        if (!args.value("process_id", Json{}).is_string() ||
            args["process_id"].get<std::string>().empty())
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "process_id is required");
        const std::string id = args["process_id"].get<std::string>();
        if (action_ == Action::Read) {
            if (!exact({"process_id", "cursor"}) ||
                (args.contains("cursor") && !args["cursor"].is_number_unsigned() &&
                 !args["cursor"].is_number_integer()))
                return MaiToolResult::failure(MaiErrorCode::InvalidInput, "invalid cursor");
            const std::int64_t cursor = args.value("cursor", std::int64_t(0));
            if (cursor < 0)
                return MaiToolResult::failure(MaiErrorCode::InvalidInput, "invalid cursor");
            const auto read = manager_->read(context.sessionId, id, std::uint64_t(cursor));
            if (!read) return failure(read.error());
            const auto& view = read.value();
            Json output{{"process_id", id}, {"cursor", view.cursor},
                        {"first_available_cursor", view.firstAvailableCursor},
                        {"output", view.output}, {"running", view.running},
                        {"has_more", view.hasMore},
                        {"older_output_discarded", view.olderOutputDiscarded}};
            if (!view.running) output["exit_code"] = view.exitCode;
            return MaiToolResult::success(output.dump(), view.hasMore ||
                                           view.olderOutputDiscarded);
        }
        if (action_ == Action::Write) {
            if (!exact({"process_id", "input"}) ||
                !args.value("input", Json{}).is_string())
                return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                              "input text is required");
            const MaiError written = manager_->write(context.sessionId, id,
                                                      args["input"].get<std::string>());
            if (written) return failure(written);
            return MaiToolResult::success(R"({"queued":true,"delivered":false})");
        }
        if (!exact({"process_id"}))
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "unexpected process parameter");
        const MaiError stopped = manager_->stop(context.sessionId, id);
        if (stopped) return failure(stopped);
        return MaiToolResult::success(R"({"stopped":true})");
    }

private:
    Action action_;
    std::shared_ptr<DesktopProcessManager> manager_;
};

} // namespace

struct DesktopProcessManager::Impl {
    mutable std::mutex mutex;
    std::map<std::string, std::shared_ptr<ProcessSession>> sessions;
    std::uint64_t nextOrder = 1;

    std::shared_ptr<ProcessSession> find(const std::string& owner,
                                         const std::string& id) const {
        std::lock_guard<std::mutex> lock(mutex);
        return ownedSession(sessions, owner, id);
    }
};

DesktopProcessManager::DesktopProcessManager() : impl_(std::make_unique<Impl>()) {}

DesktopProcessManager::~DesktopProcessManager() {
    std::vector<std::shared_ptr<ProcessSession>> sessions;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        for (const auto& pair : impl_->sessions) sessions.push_back(pair.second);
        impl_->sessions.clear();
    }
    for (const auto& session : sessions) {
        std::lock_guard<std::mutex> lock(session->mutex);
        session->stopRequested = true;
    }
    for (const auto& session : sessions)
        if (session->worker.joinable()) session->worker.join();
}

MaiResult<std::string> DesktopProcessManager::start(
    const std::string& sessionId, const std::string& workingDirectory,
    const std::string& command) {
    if (sessionId.empty() || workingDirectory.empty() || command.empty() ||
        command.size() > 8192 || command.find('\0') != std::string::npos)
        return {MaiErrorCode::InvalidInput, "Session, working directory and command are required"};
    const QFileInfo directory(QString::fromUtf8(workingDirectory.c_str()));
    if (!directory.isAbsolute() || !directory.exists() || !directory.isDir())
        return {MaiErrorCode::InvalidInput, "Session working directory does not exist"};
    auto session = std::make_shared<ProcessSession>();
    session->id = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
    session->owner = sessionId;
    session->workingDirectory = directory.canonicalFilePath();
    session->command = QString::fromUtf8(command.c_str(), int(command.size()));
    std::shared_ptr<ProcessSession> removed;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->sessions.size() >= kMaxSessions) {
            for (const auto& pair : impl_->sessions) {
                std::lock_guard<std::mutex> stateLock(pair.second->mutex);
                if (!pair.second->running && pair.second->ready &&
                    (!removed || pair.second->createdOrder < removed->createdOrder))
                    removed = pair.second;
            }
            if (!removed)
                return {MaiErrorCode::Busy, "too many running process sessions"};
            impl_->sessions.erase(removed->id);
        }
        session->createdOrder = impl_->nextOrder++;
        impl_->sessions.emplace(session->id, session);
    }
    if (removed && removed->worker.joinable()) removed->worker.join();
    session->worker = std::thread([session] { runProcess(session); });
    {
        std::unique_lock<std::mutex> lock(session->mutex);
        session->changed.wait_for(lock, std::chrono::seconds(6),
                                  [&session] { return session->ready; });
        if (session->started) return session->id;
        session->stopRequested = true;
    }
    if (session->worker.joinable()) session->worker.join();
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->sessions.erase(session->id);
    }
    return {MaiErrorCode::Internal, session->startError.empty() ?
        "process did not start" : session->startError};
}

MaiResult<DesktopProcessSnapshot> DesktopProcessManager::read(
    const std::string& sessionId, const std::string& processId,
    std::uint64_t cursor) const {
    const auto session = impl_->find(sessionId, processId);
    if (!session) return {MaiErrorCode::NotFound, "process not found in this Session"};
    std::lock_guard<std::mutex> lock(session->mutex);
    const std::uint64_t end = session->firstCursor +
        std::uint64_t(session->output.size());
    if (cursor > end) return {MaiErrorCode::InvalidInput, "cursor is beyond process output"};
    DesktopProcessSnapshot view;
    view.firstAvailableCursor = session->firstCursor;
    view.olderOutputDiscarded = cursor < session->firstCursor;
    const std::uint64_t start = (std::max)(cursor, session->firstCursor);
    const std::size_t count = std::size_t((std::min<std::uint64_t>)(
        kReadBytes, end - start));
    const QByteArray bytes = session->output.mid(int(start - session->firstCursor), int(count));
    QString decoded = QString::fromUtf8(bytes.constData(), bytes.size());
#ifdef Q_OS_WIN
    if (decoded.contains(QChar::ReplacementCharacter))
        decoded = QString::fromLocal8Bit(bytes.constData(), bytes.size());
#endif
    view.output = decoded.toUtf8().toStdString();
    view.cursor = start + count;
    view.running = session->running;
    view.hasMore = view.cursor < end;
    view.exitCode = session->exitCode;
    return view;
}

MaiError DesktopProcessManager::write(const std::string& sessionId,
    const std::string& processId, const std::string& input) {
    if (input.empty() || input.size() > 16 * 1024 ||
        input.find('\0') != std::string::npos)
        return {MaiErrorCode::InvalidInput, "input must contain 1 to 16384 text bytes"};
    const auto session = impl_->find(sessionId, processId);
    if (!session) return {MaiErrorCode::NotFound, "process not found in this Session"};
    std::lock_guard<std::mutex> lock(session->mutex);
    if (!session->running || session->stopRequested)
        return {MaiErrorCode::Busy, "process is no longer running"};
    if (session->pendingBytes + input.size() > 64 * 1024)
        return {MaiErrorCode::Busy, "process input queue is full; read output or stop it"};
    session->pendingInput.emplace_back(input.data(), int(input.size()));
    session->pendingBytes += input.size();
    return {};
}

MaiError DesktopProcessManager::stop(const std::string& sessionId,
    const std::string& processId) {
    const auto session = impl_->find(sessionId, processId);
    if (!session) return {MaiErrorCode::NotFound, "process not found in this Session"};
    std::unique_lock<std::mutex> lock(session->mutex);
    if (!session->running) return {};
    session->stopRequested = true;
    if (!session->changed.wait_for(lock, std::chrono::seconds(3),
                                   [&session] { return !session->running; }))
        return {MaiErrorCode::Busy, "process stop is still pending"};
    return {};
}

void registerDesktopProcessTools(MaiToolRegistry& registry,
    std::shared_ptr<DesktopProcessManager> manager) {
    for (const auto action : {DesktopProcessTool::Action::Start,
                              DesktopProcessTool::Action::Read,
                              DesktopProcessTool::Action::Write,
                              DesktopProcessTool::Action::Stop})
        registry.add(std::make_unique<DesktopProcessTool>(action, manager));
}
