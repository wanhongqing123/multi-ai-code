#pragma once

#include <QtGlobal>

// 共享期间阻止系统自动休眠/关屏。
//
// 为什么必须有：闲置自动锁屏会把系统切到安全桌面，画面采不到、输入注不进，
// 远程直接失联——而人在外面**没法自己解**。UAC 至少是你主动触发的，
// 锁屏是自己找上门的，实际更容易撞上。
//
// 只在共享进行时生效，析构或 release() 时立刻恢复系统默认行为。
// 不改任何系统设置、不写注册表：进程退出（哪怕是崩溃）后系统自行恢复。
class DisplaySleepBlocker {
public:
    DisplaySleepBlocker() = default;
    ~DisplaySleepBlocker();

    DisplaySleepBlocker(const DisplaySleepBlocker&) = delete;
    DisplaySleepBlocker& operator=(const DisplaySleepBlocker&) = delete;

    // 可重复调用，重复调用无副作用。
    void acquire();
    void release();

    bool isHeld() const { return held_; }

private:
    bool held_ = false;
#ifdef Q_OS_MAC
    quint32 assertionId_ = 0;
#endif
};
