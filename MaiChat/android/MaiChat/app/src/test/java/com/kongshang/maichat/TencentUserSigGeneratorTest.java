package com.kongshang.maichat;

import static org.junit.Assert.assertTrue;

import org.junit.Test;

import java.io.ByteArrayOutputStream;
import java.nio.charset.StandardCharsets;
import java.util.Base64;
import java.util.zip.Inflater;

public class TencentUserSigGeneratorTest {
    @Test
    public void generatesTencentTlsV2ZlibPayload() throws Exception {
        String value = TencentUserSigGenerator.generate(
            1_600_148_979,
            "android-user",
            "test-secret",
            604_800,
            1_700_000_000L
        );

        byte[] compressed = Base64.getDecoder().decode(
            value.replace('*', '+').replace('-', '/').replace('_', '=')
        );
        Inflater inflater = new Inflater(false);
        inflater.setInput(compressed);
        ByteArrayOutputStream output = new ByteArrayOutputStream();
        byte[] buffer = new byte[512];
        while (!inflater.finished()) {
            int count = inflater.inflate(buffer);
            if (count == 0 && inflater.needsInput()) break;
            output.write(buffer, 0, count);
        }
        inflater.end();
        String payload = output.toString(StandardCharsets.UTF_8.name());

        assertTrue(payload.contains("\"TLS.ver\":\"2.0\""));
        assertTrue(payload.contains("\"TLS.identifier\":\"android-user\""));
        assertTrue(payload.contains("\"TLS.sdkappid\":1600148979"));
        assertTrue(payload.contains("\"TLS.time\":1700000000"));
        assertTrue(payload.contains("\"TLS.sig\":"));
    }
}
