package com.kongshang.maichat;

import android.os.Handler;
import android.os.Looper;

import com.tencent.cloud.qcloudasrsdk.onesentence.QCloudOneSentenceRecognizer;
import com.tencent.cloud.qcloudasrsdk.onesentence.QCloudOneSentenceRecognizerListener;

import org.json.JSONObject;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;

/**
 * 腾讯云「一句话识别」实现。
 *
 * 选一句话识别而不是实时/录音文件识别，是因为 MaiChat 的用法就是「按住说一段、松手出结果」，
 * 正好落在它的适用范围（≤60 秒、≤3MB）。超出的走不了这个接口，这里会当场拒绝并让调用方
 * 回退成发语音消息，而不是把一个必然失败的请求发出去。
 *
 * 注意 SDK 文档与实际 aar 不一致：文档写构造函数要 AppCompatActivity，实际 aar 里有一个
 * 完全不依赖 Activity 的三参构造函数（javap 验证过）。用它可以避免持有 Activity 引用。
 */
public final class TencentSpeechRecognizer implements SpeechRecognizer {

    /** 一句话识别的硬上限：超过就必然被服务端拒绝，本地先挡掉。 */
    private static final long MAX_AUDIO_BYTES = 3L * 1024 * 1024;
    /** 中文通用引擎。电话音质场景是 8k_zh，手机录音走 16k_zh。 */
    private static final String ENGINE_TYPE = "16k_zh";

    private final String appId;
    private final String secretId;
    private final String secretKey;
    private final Handler mainHandler = new Handler(Looper.getMainLooper());

    public TencentSpeechRecognizer(String appId, String secretId, String secretKey) {
        this.appId = appId == null ? "" : appId.trim();
        this.secretId = secretId == null ? "" : secretId.trim();
        this.secretKey = secretKey == null ? "" : secretKey.trim();
    }

    @Override
    public boolean isAvailable() {
        return !appId.isEmpty() && !secretId.isEmpty() && !secretKey.isEmpty();
    }

    @Override
    public void transcribe(File audioFile, String format, Callback callback) {
        if (callback == null) return;
        if (!isAvailable()) {
            callback.onError("未配置语音识别凭证");
            return;
        }
        if (audioFile == null || !audioFile.isFile()) {
            callback.onError("录音文件不存在");
            return;
        }
        if (audioFile.length() <= 0) {
            callback.onError("录音内容为空");
            return;
        }
        if (audioFile.length() > MAX_AUDIO_BYTES) {
            callback.onError("录音超过 3MB，无法识别");
            return;
        }

        final byte[] audioData;
        try {
            audioData = readAll(audioFile);
        } catch (IOException error) {
            callback.onError("读取录音失败");
            return;
        }

        final QCloudOneSentenceRecognizer recognizer =
                new QCloudOneSentenceRecognizer(appId, secretId, secretKey);
        recognizer.setCallback(new QCloudOneSentenceRecognizerListener() {
            @Override
            public void didStartRecord() {
                // 只用「传音频数据」这条路径，不用 SDK 内置录音器，这里不会被调用。
            }

            @Override
            public void didStopRecord() {
            }

            @Override
            public void recognizeResult(QCloudOneSentenceRecognizer source, String result, Exception exception) {
                // SDK 的回调线程不保证是主线程，统一切回主线程再交给调用方碰 UI。
                mainHandler.post(() -> {
                    if (exception != null) {
                        callback.onError(describe(exception));
                        return;
                    }
                    callback.onText(extractText(result));
                });
            }
        });

        try {
            recognizer.recognize(audioData, format, ENGINE_TYPE);
        } catch (Exception error) {
            callback.onError(describe(error));
        }
    }

    /**
     * SDK 把服务端原始响应整个透传回来。成功时是 {"Response":{"Result":"...","RequestId":...}}，
     * 失败时 Response 里是 Error。解不出来就把原文交回去——排障时看得见真实报文，
     * 比吞掉换成一句「识别失败」有用得多。
     */
    static String extractText(String rawResult) {
        if (rawResult == null) return "";
        final String trimmed = rawResult.trim();
        if (trimmed.isEmpty()) return "";
        try {
            JSONObject root = new JSONObject(trimmed);
            JSONObject response = root.optJSONObject("Response");
            if (response != null) {
                if (response.has("Result")) return response.optString("Result", "").trim();
                JSONObject error = response.optJSONObject("Error");
                if (error != null) return "";
            }
            if (root.has("Result")) return root.optString("Result", "").trim();
        } catch (Exception ignored) {
            // 不是 JSON：可能 SDK 直接给了纯文本结果。
        }
        return trimmed.startsWith("{") ? "" : trimmed;
    }

    private static String describe(Exception error) {
        final String message = error == null ? null : error.getMessage();
        return message == null || message.trim().isEmpty() ? "语音识别失败" : message.trim();
    }

    private static byte[] readAll(File file) throws IOException {
        final int length = (int) file.length();
        final byte[] buffer = new byte[length];
        try (FileInputStream stream = new FileInputStream(file)) {
            int offset = 0;
            while (offset < length) {
                int read = stream.read(buffer, offset, length - offset);
                if (read < 0) throw new IOException("unexpected end of audio file");
                offset += read;
            }
        }
        return buffer;
    }
}
