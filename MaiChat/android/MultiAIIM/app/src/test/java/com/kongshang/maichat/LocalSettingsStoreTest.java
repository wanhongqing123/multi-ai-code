package com.kongshang.maichat;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

import org.junit.Test;

import java.nio.file.Files;
import java.nio.file.Path;

public class LocalSettingsStoreTest {
    @Test
    public void returnsEmptySettingsWhenFileDoesNotExist() throws Exception {
        Path root = Files.createTempDirectory("multi-ai-code-android-settings-empty");
        LocalSettingsStore store = new LocalSettingsStore(root.resolve("settings.properties").toFile());

        RemoteIMSettings settings = store.load();

        assertTrue(settings.requiresLogin());
        assertEquals("", settings.loginUserId());
    }

    @Test
    public void savesAndRestoresLoginUser() throws Exception {
        Path root = Files.createTempDirectory("multi-ai-code-android-settings");
        LocalSettingsStore store = new LocalSettingsStore(root.resolve("settings.properties").toFile());

        store.save(new RemoteIMSettings("  android-user  "));

        RemoteIMSettings restored = store.load();
        assertFalse(restored.requiresLogin());
        assertEquals("android-user", restored.loginUserId());
    }
}
