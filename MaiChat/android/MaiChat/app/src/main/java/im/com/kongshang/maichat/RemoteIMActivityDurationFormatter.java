// 文件作用：Android IM 身份、消息同步与本地历史；RemoteIMActivityDurationFormatter 负责面向界面的文本和时间格式化，本文件给出其实现。
// 主要入口：text。
package com.kongshang.maichat;

/** Shared Android rendering rule for phase and whole-task durations. */
final class RemoteIMActivityDurationFormatter {
    private RemoteIMActivityDurationFormatter() { }

    static String text(long seconds) {
        long value = Math.max(0, seconds);
        if (value < 60) return value + "秒";
        return value / 60 + "分" + value % 60 + "秒";
    }
}
