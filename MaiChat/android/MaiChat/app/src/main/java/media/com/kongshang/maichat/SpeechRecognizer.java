// 文件作用：Android 媒体播放、预览和编辑；SpeechRecognizer 负责语音识别会话与结果回调，本文件给出其实现。
// 阅读入口：从类型声明开始，依次核对用户事件、状态更新和退出时的资源收尾。
package com.kongshang.maichat;

import java.io.File;

/**
 * 语音转文字。
 *
 * 普通语音消息的可选识别接口。创作中心使用账号签发的实时 ASR；
 * 当前发布包未配置此可选识别器，普通录音保持发送语音消息的回退行为。
 */
public interface SpeechRecognizer {
    default void close() { }

    interface Callback {
        /** 识别成功。text 已 trim，可能为空串（对方没说话/全是噪音）。 */
        void onText(String text);

        /** 识别失败。message 面向用户，调用方据此决定是否回退成发语音消息。 */
        void onError(String message);
    }

    /** 当前是否可用（比如凭证没配就不可用）。不可用时调用方应直接走原有路径。 */
    boolean isAvailable();

    /**
     * 转写一段本地音频。回调在主线程执行。
     *
     * @param audioFile 本地音频文件
     * @param format    音频格式标识，如 "m4a"（对应 MediaRecorder 的 MPEG_4 + AAC）
     */
    void transcribe(File audioFile, String format, Callback callback);
}
