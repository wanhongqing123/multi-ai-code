// 文件作用：解析腾讯实时 ASR V2 的全文快照和句子更新；不依赖麦克风、网络或界面。
// 主要入口：accept、text、complete、failed；每次录音单独创建实例，签名只由账号服务生成。
package com.kongshang.maichat;

import org.json.JSONArray;
import org.json.JSONObject;
import java.util.TreeMap;

/** Tencent realtime ASR V2 wire contract; full text is replaced rather than appended. */
final class RealtimeSpeechProtocol {
    private final String voiceId;
    private final TreeMap<Integer, String> sentences = new TreeMap<>();
    private final java.util.Set<Integer> committed = new java.util.HashSet<>();
    private boolean complete, failed;
    private String text = "";

    RealtimeSpeechProtocol(String voiceId) { this.voiceId = voiceId; }

    // 迟到的旧流消息不能结束当前流；结束或失败后也不能再改写已交付的结果。
    boolean accept(JSONObject message) {
        if (complete || failed || !voiceId.equals(message.optString("voice_id", voiceId)))
            return false;
        if (message.optInt("code", -1) != 0) {
            failed = true;
            return true;
        }
        JSONObject result = message.optJSONObject("sentences");
        if (result != null) {
            JSONArray list = result.optJSONArray("sentence_list");
            boolean hasFullText = result.opt("voice_text_str") instanceof String;
            if (hasFullText || list != null) {
                // SDK V2 返回完整快照；连空字符串/空列表都必须替换旧结果。
                // 快照可修正已稳定的句子，不能沿用单句消息的 committed 锁定。
                sentences.clear();
                committed.clear();
                if (list != null) {
                    for (int i = 0; i < list.length(); ++i)
                        acceptSentence(list.optJSONObject(i));
                }
                text = hasFullText ? result.optString("voice_text_str", "") : joinedSentences();
            } else if (acceptSentence(result)) {
                // WebSocket V2 还可直接返回单句；同一 ID 的中间态只替换本句。
                text = joinedSentences();
            }
        }
        if (message.optInt("final", 0) == 1) complete = true;
        return true;
    }

    private boolean acceptSentence(JSONObject sentence) {
        if (sentence == null) return false;
        int index = sentence.optInt("sentence_id", -1);
        int type = sentence.optInt("sentence_type", -1);
        if (index < 0 || index >= 10000 || type < 0 || type > 1 ||
            !(sentence.opt("sentence") instanceof String) ||
            (committed.contains(index) && type == 0)) return false;
        sentences.put(index, sentence.optString("sentence", ""));
        if (type == 1) committed.add(index);
        return true;
    }

    boolean complete() { return complete; }
    boolean failed() { return failed; }
    String text() { return text.trim(); }
    private String joinedSentences() {
        StringBuilder text = new StringBuilder();
        for (String sentence : sentences.values()) text.append(sentence);
        return text.toString();
    }
}
