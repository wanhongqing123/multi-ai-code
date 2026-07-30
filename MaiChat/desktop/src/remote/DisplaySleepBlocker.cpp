#include "remote/DisplaySleepBlocker.h"

#ifdef Q_OS_WIN
#include <windows.h>
#elif defined(Q_OS_MAC)
#include <IOKit/pwr_mgt/IOPMLib.h>
#endif

DisplaySleepBlocker::~DisplaySleepBlocker() {
    release();
}

void DisplaySleepBlocker::acquire() {
    if (held_) return;
#ifdef Q_OS_WIN
    // ES_CONTINUOUS 让这个状态一直保持到显式清除，而不是只顶一次计时器；
    // ES_DISPLAY_REQUIRED 连屏幕一起保住——只挡休眠而让屏幕关掉的话，
    // 有些电源策略仍会随之锁屏。
    SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED);
#elif defined(Q_OS_MAC)
    IOPMAssertionID assertion = kIOPMNullAssertionID;
    const IOReturn result = IOPMAssertionCreateWithName(
        kIOPMAssertionTypePreventUserIdleDisplaySleep, kIOPMAssertionLevelOn,
        CFSTR("MaiChat remote desktop session"), &assertion);
    if (result != kIOReturnSuccess) return;
    assertionId_ = assertion;
#endif
    held_ = true;
}

void DisplaySleepBlocker::release() {
    if (!held_) return;
#ifdef Q_OS_WIN
    // 只传 ES_CONTINUOUS 即清除之前的请求，恢复系统默认的休眠/关屏策略。
    SetThreadExecutionState(ES_CONTINUOUS);
#elif defined(Q_OS_MAC)
    if (assertionId_ != kIOPMNullAssertionID) {
        IOPMAssertionRelease(static_cast<IOPMAssertionID>(assertionId_));
        assertionId_ = kIOPMNullAssertionID;
    }
#endif
    held_ = false;
}
