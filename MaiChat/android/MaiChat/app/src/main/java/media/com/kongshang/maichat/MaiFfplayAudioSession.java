// 文件作用：把 Android 音乐界面连接到三端共用的 FFplay 纯音频会话。
package com.kongshang.maichat;

final class MaiFfplayAudioSession implements AutoCloseable {
    static { System.loadLibrary("maichat_agent"); }

    private static native long nativeOpen(String path);
    private static native boolean nativeCommand(long handle, String command);
    private static native boolean nativeSeek(long handle, long positionUs);
    private static native long[] nativeStatus(long handle);
    private static native int nativeFinished(long handle);
    private static native void nativeClose(long handle);

    private long handle;

    static MaiFfplayAudioSession open(String path) {
        long handle = nativeOpen(path);
        return handle == 0 ? null : new MaiFfplayAudioSession(handle);
    }

    private MaiFfplayAudioSession(long handle) { this.handle = handle; }

    long[] status() { return handle == 0 ? null : nativeStatus(handle); }
    int finishedCode() { return handle == 0 ? Integer.MIN_VALUE : nativeFinished(handle); }
    boolean pauseToggle() { return handle != 0 && nativeCommand(handle, "pause"); }
    boolean seek(long positionUs) { return handle != 0 && nativeSeek(handle, positionUs); }

    @Override public void close() {
        long previous = handle;
        handle = 0;
        if (previous != 0) nativeClose(previous);
    }
}
