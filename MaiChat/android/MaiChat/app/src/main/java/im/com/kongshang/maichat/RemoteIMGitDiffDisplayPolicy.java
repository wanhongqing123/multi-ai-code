package com.kongshang.maichat;

import java.io.FileInputStream;
import java.io.IOException;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.Locale;

public final class RemoteIMGitDiffDisplayPolicy {
    private RemoteIMGitDiffDisplayPolicy() {
    }

    public static boolean isGitDiff(RemoteIMFileAttachment attachment) {
        if (attachment == null) return false;
        String mime = attachment.mimeType().toLowerCase(Locale.ROOT);
        return mime.contains("html")
            && expectedSha256(attachment.fileName()) != null;
    }

    public static String expectedSha256(String fileName) {
        String name = fileName == null ? "" : fileName.toLowerCase(Locale.ROOT);
        if (!name.startsWith("remote-im-diff-") || !name.endsWith(".html")) return null;
        int end = name.length() - ".html".length();
        int separator = name.lastIndexOf('-', end - 1);
        if (separator < 0) return null;
        String digest = name.substring(separator + 1, end);
        return digest.matches("[0-9a-f]{64}") ? digest : null;
    }

    public static boolean hasValidIntegrity(RemoteIMFileAttachment attachment) {
        String expected = attachment == null ? null : expectedSha256(attachment.fileName());
        if (expected == null) return false;
        try (FileInputStream input = new FileInputStream(attachment.localPath())) {
            MessageDigest digest = MessageDigest.getInstance("SHA-256");
            byte[] buffer = new byte[16 * 1024];
            for (int read; (read = input.read(buffer)) >= 0;) {
                if (read > 0) digest.update(buffer, 0, read);
            }
            StringBuilder actual = new StringBuilder(64);
            for (byte value : digest.digest()) actual.append(String.format(Locale.ROOT, "%02x", value));
            return expected.equals(actual.toString());
        } catch (IOException | NoSuchAlgorithmException error) {
            return false;
        }
    }
}
