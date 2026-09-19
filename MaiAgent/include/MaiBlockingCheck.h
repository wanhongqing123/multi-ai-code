#pragma once

// "这个线程现在不许做慢活"。
//
// 形状参考 chromium 的 base/threading/thread_restrictions.h。它把这件事说得很清楚：
//
//   Blocking call: 任何让调用线程**离开 CPU 去等**的调用。同步文件 I/O、
//   读写管道和 socket、重命名删除文件、列目录，都算。
//   抢一把低争用的锁不算。
//
// ── 我们为什么需要它 ────────────────────────────────────────────
//
// 这个工程里有一条一直只写在注释里、没人强制的约定：
//
//   MaiEventBus 的处理函数**在 publish 的那个线程上同步调用**，
//   流式期间那就是网络读线程。处理函数里不要做慢活，否则会拖慢模型读取。
//
// 光靠注释是守不住的。以后有人在事件处理函数里顺手读个文件、写一次库，表现出来不是崩溃，
// 是"模型吐字变卡了"——这种症状没人会联想到事件总线。
//
// 现在 MaiEventBus::publish 调处理函数时会套上 MaiScopedDisallowBlocking，
// 而 MaiFileSystem 的读写和 SQLite 的写入都会先问一句 maiAssertBlockingAllowed()。违反了就当场炸，
// 而不是留到线上变成玄学卡顿。
//
// ── 为什么不是只在 Debug 构建里查 ──────────────────────────────
//
// 这个项目的测试是 Release 构建的（见 CMakeLists）。只在 Debug 里查等于我们自己的测试永远查不到，
// 那这套东西就白写了。检查本身是读一个 thread_local 的 bool，和它守着的那些系统调用比，
// 开销可以忽略。

// 当前线程现在允许做慢活吗。
bool maiIsBlockingAllowed();

// 不允许时直接终止进程，并打印是谁在什么地方违反的。
//
// 终止而不是返回错误，是因为这属于**编程错误**，和越界访问同一类：返回错误的话调用方多半会忽略，
// 而症状会飘到很远的地方去。
void maiAssertBlockingAllowed(const char* what);

// 作用域内禁止慢活，离开作用域恢复原状。
//
// 可以嵌套：内层退出时恢复的是进来之前的状态，不是无条件放开。
class MaiScopedDisallowBlocking {
public:
    MaiScopedDisallowBlocking();
    ~MaiScopedDisallowBlocking();

    MaiScopedDisallowBlocking(const MaiScopedDisallowBlocking&) = delete;
    MaiScopedDisallowBlocking& operator=(const MaiScopedDisallowBlocking&) = delete;

private:
    bool mPrevious;
};

// 明确地开一个口子：这一段确实要做慢活，而且我知道自己在干什么。
//
// 对应 chromium 的 ScopedAllowBlocking。
// 用它之前先想清楚——大多数情况下正确的做法是**把慢活挪出去**（塞进队列，换个线程做），
// 而不是在这里声明豁免。
class MaiScopedAllowBlocking {
public:
    MaiScopedAllowBlocking();
    ~MaiScopedAllowBlocking();

    MaiScopedAllowBlocking(const MaiScopedAllowBlocking&) = delete;
    MaiScopedAllowBlocking& operator=(const MaiScopedAllowBlocking&) = delete;

private:
    bool mPrevious;
};
