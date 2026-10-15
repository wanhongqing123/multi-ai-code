// 文件作用：Android 本地测试；LocalSettingsStoreTest 负责覆盖对应类的状态和数据行为，本文件给出其验证。
// 运行边界：仅供自动化验证使用，不作为正式 App 或 Agent 的用户功能入口。
// 覆盖场景：returnsEmptySettingsWhenFileDoesNotExist、savesAndRestoresLoginUser。
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
        Path root = Files.createTempDirectory("maichat-android-settings-empty");
        LocalSettingsStore store = new LocalSettingsStore(root.resolve("settings.properties").toFile());

        RemoteIMSettings settings = store.load();

        assertTrue(settings.requiresLogin());
        assertEquals("", settings.loginUserId());
    }

    @Test
    public void savesAndRestoresLoginUser() throws Exception {
        Path root = Files.createTempDirectory("maichat-android-settings");
        LocalSettingsStore store = new LocalSettingsStore(root.resolve("settings.properties").toFile());

        store.save(new RemoteIMSettings("  android-user  "));

        RemoteIMSettings restored = store.load();
        assertFalse(restored.requiresLogin());
        assertEquals("android-user", restored.loginUserId());
    }
}
