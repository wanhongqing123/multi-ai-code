// 文件作用：验证腾讯 ASR V2 全文修正、句子快照、终态/错误隔离和原有 AAC 发送节奏。
// 运行边界：仅供自动化验证使用，不作为正式 App 或 Agent 的用户功能入口。
package com.kongshang.maichat;

import org.json.JSONArray;
import org.json.JSONObject;
import org.junit.Test;
import java.io.File;
import java.io.FileOutputStream;
import static org.junit.Assert.*;

public class RealtimeSpeechProtocolTest {
    private static final String VOICE = "current-recording";

    private JSONObject message(JSONObject sentences) throws Exception {
        JSONObject result = new JSONObject().put("code", 0).put("voice_id", VOICE);
        if (sentences != null) result.put("sentences", sentences);
        return result;
    }

    private JSONObject sentence(int id, int type, String text) throws Exception {
        return new JSONObject().put("sentence_id", id).put("sentence_type", type)
            .put("sentence", text);
    }

    @Test public void fullTextSnapshotsReplaceEarlierTextAndWinOverSentenceLists() throws Exception {
        RealtimeSpeechProtocol result = new RealtimeSpeechProtocol(VOICE);
        result.accept(message(new JSONObject().put("voice_text_str", "今天")));
        result.accept(message(new JSONObject().put("voice_text_str", "明天开会。")
            .put("sentence_list", new JSONArray().put(sentence(0, 1, "今天开会。")))));
        assertEquals("明天开会。", result.text());
        result.accept(message(null).put("final", 1));
        assertTrue(result.complete());
        assertFalse(result.accept(message(new JSONObject().put("voice_text_str", "迟到结果"))));
        assertEquals("明天开会。", result.text());
    }

    @Test public void sentenceListSnapshotsSortIdsReplaceCorrectionsAndRemoveOldSentences() throws Exception {
        RealtimeSpeechProtocol result = new RealtimeSpeechProtocol(VOICE);
        result.accept(message(new JSONObject().put("sentence_list", new JSONArray()
            .put(sentence(1, 0, "请准时。" )).put(sentence(0, 1, "今天开会。")))));
        assertEquals("今天开会。请准时。", result.text());
        result.accept(message(new JSONObject().put("sentence_list", new JSONArray()
            .put(sentence(0, 1, "明天开会。")))));
        assertEquals("明天开会。", result.text());
        result.accept(message(new JSONObject().put("sentence_list", new JSONArray())));
        assertEquals("", result.text());
    }

    @Test public void websocketSentenceUpdatesReplaceTheirIdAndKeepStableText() throws Exception {
        RealtimeSpeechProtocol result = new RealtimeSpeechProtocol(VOICE);
        result.accept(message(sentence(0, 0, "今天")));
        result.accept(message(sentence(0, 1, "今天开会。")));
        result.accept(message(sentence(0, 0, "旧中间态")));
        result.accept(message(sentence(1, 1, "请准时。" )).put("final", 1));
        assertEquals("今天开会。请准时。", result.text());
        assertTrue(result.complete());
    }

    @Test public void explicitEmptyTextClearsTheTranscriptAndAnEmptyFinalIsSuccessful() throws Exception {
        RealtimeSpeechProtocol result = new RealtimeSpeechProtocol(VOICE);
        result.accept(message(new JSONObject().put("voice_text_str", "背景噪音")));
        result.accept(message(new JSONObject().put("voice_text_str", "")
            .put("sentence_list", new JSONArray().put(sentence(0, 1, "忽略列表")))));
        result.accept(message(null).put("final", 1));
        assertEquals("", result.text());
        assertTrue(result.complete());
        assertFalse(result.failed());
    }

    @Test public void errorsDoNotPublishTextOrFinalAndCannotBeRevived() throws Exception {
        RealtimeSpeechProtocol result = new RealtimeSpeechProtocol(VOICE);
        result.accept(message(new JSONObject().put("voice_text_str", "中间态")));
        result.accept(message(new JSONObject().put("voice_text_str", "错误正文"))
            .put("code", 4008).put("final", 1));
        assertTrue(result.failed());
        assertFalse(result.complete());
        assertEquals("中间态", result.text());
        assertFalse(result.accept(message(null).put("final", 1)));
        assertFalse(result.complete());
    }

    @Test public void otherRecordingsAndV1ResultsCannotChangeThisV2Session() throws Exception {
        RealtimeSpeechProtocol result = new RealtimeSpeechProtocol(VOICE);
        assertFalse(result.accept(message(new JSONObject().put("voice_text_str", "旧录音"))
            .put("voice_id", "previous-recording").put("final", 1)));
        assertFalse(result.accept(message(null).put("voice_id", "previous-recording").put("code", 4008)));
        result.accept(message(null).put("result", new JSONObject().put("index", 0)
            .put("slice_type", 2).put("voice_text_str", "旧协议")));
        assertEquals("", result.text());
        assertFalse(result.complete());
        assertFalse(result.failed());
        result.accept(message(new JSONObject().put("voice_text_str", "本次录音")));
        RealtimeSpeechProtocol next = new RealtimeSpeechProtocol("next-recording");
        assertFalse(next.accept(message(null).put("final", 1)));
        assertEquals("", next.text());
        assertFalse(next.complete());
    }
    private byte[] frame() {
        int size = 23;
        byte[] data = new byte[size];
        data[0] = (byte) 0xff; data[1] = (byte) 0xf1; data[2] = 0x60;
        data[3] = 0x40; data[4] = (byte) (size >> 3); data[5] = (byte) (((size & 7) << 5) | 31); data[6] = (byte) 0xfc;
        return data;
    }
    @Test public void incompleteFramesWaitForMoreDataAndTheBudgetPreventsBursts() throws Exception {
        File file = File.createTempFile("maichat-aac", ".aac"); byte[] frame = frame();
        try (FileOutputStream output = new FileOutputStream(file); AdtsFrameReader reader = new AdtsFrameReader(file)) {
            output.write(frame, 0, 10); output.flush();
            assertEquals(0, reader.readUntilSample(3200).length);
            output.write(frame, 10, frame.length - 10);
            for (int count = 0; count < 9; count++) output.write(frame);
            output.flush();
            assertEquals(3 * frame.length, reader.readUntilSample(3200).length);
            assertEquals(3072, reader.samples()); assertFalse(reader.atEnd());
            assertEquals(0, reader.readUntilSample(3200).length);
            assertEquals(7 * frame.length, reader.readUntilSample(10240).length);
            assertTrue(reader.atEnd());
        } finally { file.delete(); }
    }
}
