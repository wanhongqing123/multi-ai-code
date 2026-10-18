// 文件作用：Android 媒体播放、预览和编辑；RealtimeSpeechProtocol 负责协议字段、编码解码及版本兼容，本文件给出其实现。
// 主要入口：accept、text、complete；签名只由账号服务生成。
package com.kongshang.maichat;

import org.json.JSONObject;
import java.util.TreeMap;

/** Tencent realtime ASR v1 wire contract; no microphone, socket or UI dependencies. */
final class RealtimeSpeechProtocol {
    private final TreeMap<Integer, String> sentences = new TreeMap<>();
    private final java.util.Set<Integer> committed = new java.util.HashSet<>();
    private boolean complete;
    void accept(JSONObject message) {
        JSONObject result = message.optJSONObject("result");
        if (result != null) {
            int index = result.optInt("index", -1);
            int type = result.optInt("slice_type", -1);
            if (index >= 0 && index < 10000 && type >= 0 && type <= 2 && !committed.contains(index)) {
                sentences.put(index, result.optString("voice_text_str", ""));
                if (type == 2) committed.add(index);
            }
        }
        if (message.optInt("final", 0) == 1) complete = true;
    }
    boolean complete() { return complete; }
    String text() {
        StringBuilder text = new StringBuilder();
        for (String sentence : sentences.values()) text.append(sentence);
        return text.toString().trim();
    }
}
