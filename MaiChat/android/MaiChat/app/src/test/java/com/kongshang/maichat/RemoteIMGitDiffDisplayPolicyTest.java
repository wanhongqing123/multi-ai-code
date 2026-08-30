package com.kongshang.maichat;

import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

import java.io.File;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;

import org.junit.Test;

public final class RemoteIMGitDiffDisplayPolicyTest {
    @Test
    public void onlyRecognizesAndVerifiesGeneratedHtmlDiffArtifacts() throws Exception {
        byte[] content = "<html>diff</html>".getBytes(StandardCharsets.UTF_8);
        StringBuilder digest = new StringBuilder(64);
        for (byte value : MessageDigest.getInstance("SHA-256").digest(content)) {
            digest.append(String.format("%02x", value));
        }
        File directory = new File(System.getProperty("java.io.tmpdir"), "maichat-diff-policy");
        assertTrue(directory.mkdirs() || directory.isDirectory());
        File file = new File(directory, "remote-im-diff-repo-" + digest + ".html");
        try (FileOutputStream output = new FileOutputStream(file)) {
            output.write(content);
        }
        RemoteIMFileAttachment diff = new RemoteIMFileAttachment(
            file.getAbsolutePath(),
            file.getName(),
            "text/html",
            4096
        );
        assertTrue(RemoteIMGitDiffDisplayPolicy.isGitDiff(diff));
        assertTrue(RemoteIMGitDiffDisplayPolicy.hasValidIntegrity(diff));
        assertFalse(RemoteIMGitDiffDisplayPolicy.isGitDiff(new RemoteIMFileAttachment(
            "/tmp/report.html",
            "report.html",
            "text/html",
            4096
        )));
        try (FileOutputStream output = new FileOutputStream(file, true)) {
            output.write('!');
        }
        assertFalse(RemoteIMGitDiffDisplayPolicy.hasValidIntegrity(diff));
        assertTrue(file.delete());
    }
}
