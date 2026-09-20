#include "MaiProcess.h"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

// iOS / tvOS / watchOS 上 app 不允许 exec 别的可执行文件，也没有 shell。
// 这是沙箱的硬规矩，不是权限没配对，所以这些平台整个实现换成一句「不支持」。
#if (defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE) || \
    (defined(TARGET_OS_TV) && TARGET_OS_TV) || (defined(TARGET_OS_WATCH) && TARGET_OS_WATCH)
#define MAI_PROCESS_EXECUTION_SUPPORTED 0
#else
#define MAI_PROCESS_EXECUTION_SUPPORTED 1
#endif

#if MAI_PROCESS_EXECUTION_SUPPORTED

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

#include "MaiBlockingCheck.h"

namespace {

// 轮询间隔。太短空转烧 CPU，太长「停止」按下去会迟钝。
constexpr int kPollIntervalMs = 20;

// 把子进程放进**自己的进程组**，杀的时候连它拉起来的孙子进程一起杀。
//
// 不这么做的话 `sh -c "sleep 300 &"` 超时后只有 sh 被杀，sleep 变成孤儿继续跑，
// 而且它还攥着管道的写端，我们读到 EOF 的时机跟着被拖住。
void killProcessGroup(pid_t pid) {
    ::kill(-pid, SIGKILL);
    ::kill(pid, SIGKILL);
}

void appendCapped(std::string& out, const char* data, std::size_t size, std::size_t cap,
                  bool& truncated) {
    if (out.size() >= cap) {
        truncated = true;
        return;
    }
    const std::size_t room = cap - out.size();
    out.append(data, std::min(room, size));
    if (size > room) truncated = true;
}

}  // namespace

bool maiIsProcessExecutionSupported() {
    return true;
}

MaiError maiRunProcess(const MaiProcessOptions& options, MaiProcessResult& result) {
    maiAssertBlockingAllowed("maiRunProcess");
    result = MaiProcessResult{};
    if (options.argv.empty()) {
        return MaiError(MaiErrorCode::InvalidInput, "no command given");
    }

    // 用 fork + exec，不用 posix_spawn。
    //
    // posix_spawn 要设工作目录得靠 posix_spawn_file_actions_addchdir_np——名字里的 _np
    // 就是 non-portable：老的 glibc、musl、以及 API 34 以前的 Android bionic 都没有。
    // 这个库要能跑在 Android 和嵌入式上，不能挑这种扩展。
    // fork 之后在子进程里直接 chdir 是纯 POSIX，哪儿都有。
    std::vector<std::string> storage;
    if (options.useShell) {
        std::string joined;
        for (std::size_t i = 0; i < options.argv.size(); ++i) {
            if (i > 0) joined += ' ';
            joined += options.argv[i];
        }
        storage = {"/bin/sh", "-c", joined};
    } else {
        storage = options.argv;
    }

    int pipeFds[2] = {-1, -1};
    if (::pipe(pipeFds) != 0) {
        return MaiError(MaiErrorCode::Internal, std::string("pipe failed: ") + std::strerror(errno));
    }
    // exec 成功时这个管道会被自动关掉；失败时子进程往里写 errno，父进程据此知道
    // 「启动失败」而不是「命令跑了但没输出」。没有这条通道的话两者分不开。
    int errorFds[2] = {-1, -1};
    if (::pipe(errorFds) != 0) {
        ::close(pipeFds[0]);
        ::close(pipeFds[1]);
        return MaiError(MaiErrorCode::Internal, std::string("pipe failed: ") + std::strerror(errno));
    }
    ::fcntl(errorFds[1], F_SETFD, FD_CLOEXEC);

    std::vector<char*> argv;
    argv.reserve(storage.size() + 1);
    for (std::string& piece : storage) argv.push_back(piece.data());
    argv.push_back(nullptr);

    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(pipeFds[0]);
        ::close(pipeFds[1]);
        ::close(errorFds[0]);
        ::close(errorFds[1]);
        return MaiError(MaiErrorCode::Internal, std::string("fork failed: ") + std::strerror(errno));
    }

    if (pid == 0) {
        // 子进程。**这里只能调 async-signal-safe 的东西**——fork 之后、exec 之前，
        // 别的线程持有的锁在这个进程里永远不会被释放，碰 malloc 之类就可能死锁。
        // 下面用到的 chdir / dup2 / close / open / setpgid / execvp / write / _exit 都是安全的。
        ::setpgid(0, 0);
        if (!options.workingDirectory.empty() &&
            ::chdir(options.workingDirectory.c_str()) != 0) {
            const int failure = errno;
            ssize_t ignored = ::write(errorFds[1], &failure, sizeof(failure));
            (void)ignored;
            ::_exit(127);
        }
        // stdin 接 /dev/null。不接的话子进程等输入会永远卡着，我们只能等到超时——
        // 用户看到的是「命令挂了」，实际是它在等一个永远不会来的回车。
        const int devNull = ::open("/dev/null", O_RDONLY);
        if (devNull >= 0) {
            ::dup2(devNull, STDIN_FILENO);
            ::close(devNull);
        }
        // stdout 和 stderr 都接到同一个写端：两股合成一股，顺序就是终端里看到的顺序。
        ::dup2(pipeFds[1], STDOUT_FILENO);
        ::dup2(pipeFds[1], STDERR_FILENO);
        ::close(pipeFds[0]);
        ::close(pipeFds[1]);
        ::execvp(argv[0], argv.data());

        const int failure = errno;
        ssize_t ignored = ::write(errorFds[1], &failure, sizeof(failure));
        (void)ignored;
        ::_exit(127);
    }

    ::close(pipeFds[1]);
    ::close(errorFds[1]);

    int startupErrno = 0;
    const ssize_t errorBytes = ::read(errorFds[0], &startupErrno, sizeof(startupErrno));
    ::close(errorFds[0]);
    if (errorBytes == static_cast<ssize_t>(sizeof(startupErrno))) {
        ::close(pipeFds[0]);
        int discarded = 0;
        ::waitpid(pid, &discarded, 0);
        return MaiError(MaiErrorCode::NotFound,
                        "could not start \"" + storage[0] + "\": " + std::strerror(startupErrno));
    }

    const int flags = ::fcntl(pipeFds[0], F_GETFL, 0);
    ::fcntl(pipeFds[0], F_SETFL, flags | O_NONBLOCK);

    const auto started = std::chrono::steady_clock::now();
    char buffer[4096];
    bool reaped = false;
    int status = 0;

    while (true) {
        const ssize_t got = ::read(pipeFds[0], buffer, sizeof(buffer));
        if (got > 0) {
            // 超了上限就不再往里塞，但**照常继续读**：不读的话子进程会卡在写管道上，
            // 于是它永远不退出，我们只能等超时。
            appendCapped(result.output, buffer, static_cast<std::size_t>(got),
                         options.maxOutputBytes, result.truncated);
            continue;
        }
        if (got == 0) break;  // 写端全关了，输出到头了
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) break;

        if (::waitpid(pid, &status, WNOHANG) == pid) {
            reaped = true;
            // 进程没了，但管道里可能还剩没读完的字节，抽干再走。
            while (true) {
                const ssize_t rest = ::read(pipeFds[0], buffer, sizeof(buffer));
                if (rest <= 0) break;
                appendCapped(result.output, buffer, static_cast<std::size_t>(rest),
                             options.maxOutputBytes, result.truncated);
            }
            break;
        }

        if (options.cancel != nullptr && options.cancel->load(std::memory_order_relaxed)) {
            result.canceled = true;
            killProcessGroup(pid);
            break;
        }
        if (options.timeoutMs > 0) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - started)
                                     .count();
            if (elapsed >= options.timeoutMs) {
                result.timedOut = true;
                killProcessGroup(pid);
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
    }

    ::close(pipeFds[0]);
    if (!reaped) ::waitpid(pid, &status, 0);

    if (WIFEXITED(status)) {
        result.exitCode = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.exitCode = -WTERMSIG(status);
    }
    return MaiError();
}

#else  // MAI_PROCESS_EXECUTION_SUPPORTED

bool maiIsProcessExecutionSupported() {
    return false;
}

MaiError maiRunProcess(const MaiProcessOptions& options, MaiProcessResult& result) {
    (void)options;
    result = MaiProcessResult{};
    return MaiError(MaiErrorCode::NotSupported,
                    "this platform does not allow running external processes");
}

#endif  // MAI_PROCESS_EXECUTION_SUPPORTED
