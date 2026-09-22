package com.kongshang.maichat;

import org.json.JSONObject;
import org.junit.Test;
import java.io.File;
import java.io.FileOutputStream;
import static org.junit.Assert.*;

public class RealtimeSpeechProtocolTest {
    @Test public void partialUpdatesReplaceTheirSentenceInsteadOfDuplicatingIt() throws Exception {
        RealtimeSpeechProtocol result = new RealtimeSpeechProtocol();
        result.accept(new JSONObject("{\"code\":0,\"result\":{\"index\":0,\"slice_type\":1,\"voice_text_str\":\"今天\"}}"));
        result.accept(new JSONObject("{\"code\":0,\"result\":{\"index\":0,\"slice_type\":2,\"voice_text_str\":\"今天开会。\"}}"));
        result.accept(new JSONObject("{\"code\":0,\"result\":{\"index\":0,\"slice_type\":1,\"voice_text_str\":\"旧结果\"}}"));
        result.accept(new JSONObject("{\"code\":0,\"result\":{\"index\":1,\"slice_type\":2,\"voice_text_str\":\"请准时。\"},\"final\":1}"));
        assertEquals("今天开会。请准时。", result.text()); assertTrue(result.complete());
    }
    @Test public void signatureMatchesAnIndependentHmacFixtureAndEscapesBase64() throws Exception {
        String url = RealtimeSpeechProtocol.signedUrl("123456789", "AKID_TEST_ONLY", "test-secret-key", "test-voice", 1700000000L, 12345);
        assertTrue(url.endsWith("&signature=KXqGp%2BsgYzdyVAiirIbDGnh9NjM%3D"));
        assertTrue(url.contains("voice_format=16"));
        assertTrue(url.contains("expired=1700003600"));
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
