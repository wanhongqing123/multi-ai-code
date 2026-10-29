// 文件作用：只为独立 QtTest 进程走真实 macOS 前台激活；不改生产 App 的安全门禁。
#import <AppKit/AppKit.h>
#include <QString>

void activateDesktopPasswordTestApplication() {
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    [NSApp activateIgnoringOtherApps:YES];
}

QString desktopPasswordTestNativeState() {
    NSRunningApplication* current = [NSRunningApplication currentApplication];
    NSRunningApplication* front = [[NSWorkspace sharedWorkspace] frontmostApplication];
    return QStringLiteral("ns_active=%1 ns_running=%2 activation_policy=%3 self_pid=%4 front_pid=%5")
        .arg([NSApp isActive]).arg([NSApp isRunning]).arg(int([NSApp activationPolicy]))
        .arg(int([current processIdentifier])).arg(int([front processIdentifier]));
}
