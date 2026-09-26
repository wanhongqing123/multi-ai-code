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
