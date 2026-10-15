// 文件作用：Android 设备集成测试；DiagnosticExportInstrumentedTest 负责覆盖对应界面或服务的设备行为，本文件给出其验证。
// 运行边界：仅供自动化验证使用，不作为正式 App 或 Agent 的用户功能入口。
// 覆盖场景：exportsAWorkspaceFileWithoutReturningLogContents。
package com.kongshang.maichat;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertTrue;

import androidx.test.core.app.ActivityScenario;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import org.json.JSONObject;
import org.junit.Test;
import org.junit.runner.RunWith;

@RunWith(AndroidJUnit4.class)
public class DiagnosticExportInstrumentedTest {
    @Test public void exportsAWorkspaceFileWithoutReturningLogContents() throws Exception {
        try (ActivityScenario<MainActivity> scenario = ActivityScenario.launch(MainActivity.class)) {
            AtomicReference<MainActivity> current = new AtomicReference<>();
            scenario.onActivity(current::set);
            MainActivity activity = current.get();
            assertNotNull(activity);
            AIAssistantController agent = AIAssistantController.shared(activity);
            long deadline = System.currentTimeMillis() + 15_000;
            while (System.currentTimeMillis() < deadline) {
                try {
                    agent.workspaceDiagnosticOutputForHost();
                    break;
                } catch (IllegalStateException unavailable) {
                    Thread.sleep(100);
                }
            }
            ExecutorService worker = Executors.newSingleThreadExecutor();
            File report = null;
            try {
                Future<JSONObject> task = worker.submit(activity::exportDiagnosticsForAgent);
                JSONObject result = task.get(35, TimeUnit.SECONDS);
                assertEquals("text/plain", result.getString("mime_type"));
                assertFalse(result.has("content"));
                report = agent.workspaceFile(result.getString("path"));
                assertNotNull(report);
                assertTrue(report.isFile());
                assertEquals(report.length(), result.getLong("bytes"));
                String text = new String(Files.readAllBytes(report.toPath()),
                    StandardCharsets.UTF_8);
                assertTrue(text.startsWith("MaiChat Android\nversion="));
            } finally {
                if (report != null) Files.deleteIfExists(report.toPath());
                worker.shutdownNow();
            }
        }
    }
}
