#include "MaiProcess.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <thread>
#include <vector>

#include "MaiBlockingCheck.h"
#include "MaiFilePath.h"

namespace {

constexpr int kPollIntervalMs = 20;

std::wstring toWide(const std::string& utf8) {
    if (utf8.empty()) return std::wstring();
    const int needed =
        ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(needed), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(),
                          needed);
    return wide;
}

// 一个参数在 Windows 命令行里的转义。
//
// Windows 没有 execv：CreateProcess 收的是**一整行字符串**，由子进程自己按
// CommandLineToArgvW 的规则拆回来。所以我们必须按那套规则反向拼，不然参数里
// 带空格或引号就会被拆错——模型搜一个带空格的字符串就会变成两个参数。
//
// 规则来自 MSDN 的 "Parsing C++ Command-Line Arguments"：反斜杠只有在引号前面
// 才需要翻倍。
std::wstring quoteArgument(const std::wstring& argument) {
    if (!argument.empty() &&
        argument.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        return argument;
    }
    std::wstring quoted = L"\"";
    for (auto it = argument.begin();; ++it) {
        std::size_t backslashes = 0;
        while (it != argument.end() && *it == L'\\') {
            ++it;
            ++backslashes;
        }
        if (it == argument.end()) {
            quoted.append(backslashes * 2, L'\\');
            break;
        }
        if (*it == L'"') {
            quoted.append(backslashes * 2 + 1, L'\\');
        } else {
            quoted.append(backslashes, L'\\');
        }
        quoted.push_back(*it);
    }
    quoted.push_back(L'"');
    return quoted;
}

// 杀整棵进程树。
//
// TerminateProcess 只杀自己，子进程照样跑。所以把子进程放进一个 Job 对象，
// 关掉 Job 就把里面所有进程一起带走——这是 Windows 上唯一可靠的办法。
void killJob(HANDLE job) {
    if (job != nullptr) ::TerminateJobObject(job, 1);
}

}  // namespace

bool maiIsProcessExecutionSupported() {
    // 桌面 Windows 可以。UWP / WinRT 沙箱里 CreateProcess 是被禁的，
    // 真要上那种环境时这里要跟着分支——现在没有那个目标，不提前写。
    return true;
}

MaiError maiRunProcess(const MaiProcessOptions& options, MaiProcessResult& result) {
    maiAssertBlockingAllowed("maiRunProcess");
    result = MaiProcessResult{};
    if (options.argv.empty()) {
        return MaiError(MaiErrorCode::InvalidInput, "no command given");
    }

    SECURITY_ATTRIBUTES inherit{};
    inherit.nLength = sizeof(inherit);
    inherit.bInheritHandle = TRUE;

    HANDLE readEnd = nullptr;
    HANDLE writeEnd = nullptr;
    if (!::CreatePipe(&readEnd, &writeEnd, &inherit, 0)) {
        return MaiError(MaiErrorCode::Internal, "CreatePipe failed");
    }
    // 读端不能被子进程继承，否则我们永远等不到 EOF——子进程手里还攥着一个副本。
    ::SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);

    std::wstring commandLine;
    if (options.useShell) {
        std::string joined;
        for (std::size_t i = 0; i < options.argv.size(); ++i) {
            if (i > 0) joined += ' ';
            joined += options.argv[i];
        }
        commandLine = L"cmd.exe /c " + toWide(joined);
    } else {
        for (std::size_t i = 0; i < options.argv.size(); ++i) {
            if (i > 0) commandLine.push_back(L' ');
            commandLine += quoteArgument(toWide(options.argv[i]));
        }
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = writeEnd;
    startup.hStdError = writeEnd;
    // stdin 给一个空设备：子进程等输入时立刻拿到 EOF，而不是挂在那儿等超时。
    HANDLE nullInput = ::CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ, &inherit, OPEN_EXISTING,
                                     0, nullptr);
    startup.hStdInput = nullInput;

    const std::wstring workingDirectory = toWide(options.workingDirectory);
    std::vector<wchar_t> mutableCommandLine(commandLine.begin(), commandLine.end());
    mutableCommandLine.push_back(L'\0');

    PROCESS_INFORMATION process{};
    const BOOL started = ::CreateProcessW(
        nullptr, mutableCommandLine.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_NEW_PROCESS_GROUP, nullptr,
        workingDirectory.empty() ? nullptr : workingDirectory.c_str(), &startup, &process);

    ::CloseHandle(writeEnd);
    if (nullInput != INVALID_HANDLE_VALUE) ::CloseHandle(nullInput);
    if (!started) {
        ::CloseHandle(readEnd);
        return MaiError(MaiErrorCode::NotFound, "could not start \"" + options.argv[0] + "\"");
    }

    // Job 要在进程**还挂起**的时候挂上去，不然它可能已经派生出不受管的孙子进程了。
    HANDLE job = ::CreateJobObjectW(nullptr, nullptr);
    if (job != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        ::SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
        ::AssignProcessToJobObject(job, process.hProcess);
    }
    ::ResumeThread(process.hThread);

    const auto begun = std::chrono::steady_clock::now();
    char buffer[4096];
    while (true) {
        DWORD available = 0;
        if (::PeekNamedPipe(readEnd, nullptr, 0, nullptr, &available, nullptr) && available > 0) {
            DWORD got = 0;
            const DWORD want = static_cast<DWORD>(std::min<std::size_t>(sizeof(buffer), available));
            if (!::ReadFile(readEnd, buffer, want, &got, nullptr) || got == 0) break;
            // 超了上限就不再收，但照常把管道抽干：不抽的话子进程会卡在写管道上，
            // 于是它永远不退出，我们只能等超时。
            if (result.output.size() < options.maxOutputBytes) {
                const std::size_t room = options.maxOutputBytes - result.output.size();
                result.output.append(buffer, std::min<std::size_t>(room, got));
                if (got > room) result.truncated = true;
            } else {
                result.truncated = true;
            }
            continue;
        }

        const DWORD waited = ::WaitForSingleObject(process.hProcess, 0);
        if (waited == WAIT_OBJECT_0) {
            // 进程没了，管道里可能还有剩的，抽干再走。
            DWORD rest = 0;
            while (::PeekNamedPipe(readEnd, nullptr, 0, nullptr, &rest, nullptr) && rest > 0) {
                DWORD got = 0;
                const DWORD want = static_cast<DWORD>(std::min<std::size_t>(sizeof(buffer), rest));
                if (!::ReadFile(readEnd, buffer, want, &got, nullptr) || got == 0) break;
                if (result.output.size() < options.maxOutputBytes) {
                    const std::size_t room = options.maxOutputBytes - result.output.size();
                    result.output.append(buffer, std::min<std::size_t>(room, got));
                    if (got > room) result.truncated = true;
                } else {
                    result.truncated = true;
                }
            }
            break;
        }

        if (options.cancel != nullptr && options.cancel->load(std::memory_order_relaxed)) {
            result.canceled = true;
            killJob(job);
            break;
        }
        if (options.timeoutMs > 0) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - begun)
                                     .count();
            if (elapsed >= options.timeoutMs) {
                result.timedOut = true;
                killJob(job);
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
    }

    ::WaitForSingleObject(process.hProcess, 2000);
    DWORD exitCode = 0;
    ::GetExitCodeProcess(process.hProcess, &exitCode);
    result.exitCode = static_cast<int>(exitCode);

    ::CloseHandle(readEnd);
    ::CloseHandle(process.hThread);
    ::CloseHandle(process.hProcess);
    if (job != nullptr) ::CloseHandle(job);
    return MaiError();
}
