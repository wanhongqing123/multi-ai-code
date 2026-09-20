#pragma once

#include <atomic>
#include <cstddef>
#include <string>
#include <vector>

#include "MaiError.h"

// 跑一个外部命令，把它说的话收回来。
//
// ── 为什么不是 system() 或 popen() ──────────────────────────────
//
// 两个都会把命令交给一个 shell 去解释，于是参数里的 `;`、`&&`、反引号、`$(...)`
// 都成了可执行的东西。模型给的命令本来就可能被提示词注入污染，再套一层 shell
// 等于把注入点直接接到系统上。这里**自己建进程**，参数原样传给子进程，
// 不经过任何 shell 的语法解析。
//
// 需要 shell 语法的时候由调用方显式说（MaiProcessOptions::useShell），
// 那是一个看得见的决定，不是默认行为。
//
// ── 契约 ────────────────────────────────────────────────────────
//
// 这是**阻塞调用**，会一直等到子进程退出、超时、或者被取消。所以它只能在
// 那一轮对话的工作线程上调——绝对不能在事件总线的处理函数里调，
// 那里跑着网络读线程，MaiBlockingCheck 会当场炸（这是它存在的意义）。

struct MaiProcessOptions {
    // 要跑的程序和参数。**argv[0] 是程序名**，和 execv 一致。
    std::vector<std::string> argv;

    // 子进程的工作目录，UTF-8 绝对路径。空表示继承当前进程的——
    // 但工具层不该给空：那会让模型在进程的当前目录里跑命令，而那通常不是用户的项目。
    std::string workingDirectory;

    // 超时。到点就杀掉整棵进程树，并把 timedOut 置位。
    // 0 表示不限时——**工具层不要传 0**，模型写出 `tail -f` 这种命令是常事。
    int timeoutMs = 120000;

    // 收多少输出。超过就截断并置位 truncated，但子进程照常跑完，
    // 不会因为我们不读了就卡在写管道上。
    std::size_t maxOutputBytes = 64 * 1024;

    // 走系统 shell 解释整行命令（Windows 上是 cmd /c，其余是 /bin/sh -c）。
    //
    // **默认关**。打开之后管道、重定向、`&&` 这些才有效，代价是参数里的元字符
    // 也跟着生效。要打开就必须在调用点写清楚为什么。
    bool useShell = false;

    // 指向那一轮的取消标志，可能为空。跑得久的命令靠它响应「停止」——
    // 不看的话用户按了停还得等命令自己跑完。
    const std::atomic<bool>* cancel = nullptr;
};

struct MaiProcessResult {
    // 子进程的退出码。被信号杀掉时是负的信号值（POSIX），
    // 超时被我们杀掉时没有意义，看 timedOut。
    int exitCode = 0;

    // stdout 和 stderr **合成一股**，按到达顺序交错。
    //
    // 分开收对模型没好处：它要的是「这条命令说了什么」，而编译器把错误写在
    // stderr、进度写在 stdout，分开之后对不上号。交错着给才是终端里看到的样子。
    std::string output;

    bool timedOut = false;
    bool canceled = false;
    // 输出超过 maxOutputBytes，尾巴被丢了。**一定要如实往上报**：
    // 不报的话模型会把看到的当成全部（见 MaiToolResult::isTruncated 的注释）。
    bool truncated = false;

    bool isSuccess() const;
};

// 这个平台跑不跑得了外部进程。
//
// **不是所有平台都行。** iOS 的沙箱不允许 app 去 exec 别的可执行文件（也没有 shell），
// 这是 App Store 的硬规矩，不是权限没给对；嵌入式上也可能压根没有进程这个概念。
// Android 可以（bionic 有 /system/bin/sh），但 app 沙箱之外的东西照样看不见。
//
// 调用方**必须先问这个**。registerMaiBuiltinTools 就是靠它决定要不要把 shell 工具
// 摆给模型看——摆一个永远失败的工具比不摆更糟：模型会反复试，而它收到的
// 「失败」听起来像临时故障，于是它换个写法再试一遍。
bool maiIsProcessExecutionSupported();

// 跑一个命令。启动失败（程序不存在、工作目录不存在）返回带错误码的 MaiError；
// 平台压根不支持时返回 NotSupported。
// **命令自己跑失败不算错误**——那是正常结果，看 exitCode。
MaiError maiRunProcess(const MaiProcessOptions& options, MaiProcessResult& result);
