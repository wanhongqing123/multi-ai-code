package com.kongshang.maichat;

import java.io.File;

/**
 * 语音转文字。
 *
 * 单独抽一层接口，是为了把「凭证从哪来」挡在调用方之外：现在是 local.properties 注入的
 * 长期密钥（仅够本机联调），将来换成 STS 临时密钥、或改由桌面端代理转发，都只换实现，
 * MainActivity 不用动。
 */
public interface SpeechRecognizer {

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
