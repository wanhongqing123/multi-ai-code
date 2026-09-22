package com.kongshang.maichat;

import static org.junit.Assert.*;

import android.content.Context;
import android.graphics.Bitmap;
import android.view.View;
import android.view.ViewGroup;
import android.widget.TextView;
import androidx.test.core.app.ActivityScenario;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import java.io.File;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import org.junit.Test;
import org.junit.runner.RunWith;

@RunWith(AndroidJUnit4.class)
public class AIAssistantInstrumentedTest {
    private interface Check {
        boolean check(AIAssistantPanel panel) throws Exception;
    }
    private void await(ActivityScenario<AIAssistantTestActivity> scenario, Check check) throws Exception {
        long deadline = System.currentTimeMillis() + 15000;
        AtomicBoolean passed = new AtomicBoolean();
        while (!passed.get() && System.currentTimeMillis() < deadline) {
            scenario.onActivity(a -> {
                try {
                    passed.set(check.check(a.panel));
                } catch (Exception e) {
                    throw new AssertionError(e);
                }
            });
            Thread.sleep(50);
        }
        assertTrue("AI UI condition timed out", passed.get());
    }
    private boolean visibleText(View view, String text) {
        if (view instanceof TextView && ((TextView) view).getText().toString().contains(text)) {
            android.graphics.Rect bounds = new android.graphics.Rect();
            return view.getGlobalVisibleRect(bounds) && bounds.height() > 0;
        }
        if (view instanceof ViewGroup)
            for (int i = 0; i < ((ViewGroup) view).getChildCount(); i++)
                if (visibleText(((ViewGroup) view).getChildAt(i), text))
                    return true;
        return false;
    }
    private View findDescription(View view, String description) {
        if (description.contentEquals(
                view.getContentDescription() == null ? "" : view.getContentDescription()))
            return view;
        if (view instanceof ViewGroup)
            for (int i = 0; i < ((ViewGroup) view).getChildCount(); i++) {
                View result = findDescription(((ViewGroup) view).getChildAt(i), description);
                if (result != null)
                    return result;
            }
        return null;
    }
    private View findList(View view) {
        if (view instanceof android.widget.ListView)
            return view;
        if (view instanceof ViewGroup)
            for (int i = 0; i < ((ViewGroup) view).getChildCount(); i++) {
                View result = findList(((ViewGroup) view).getChildAt(i));
                if (result != null)
                    return result;
            }
        return null;
    }
    @Test
    public void streamsUnicodeStopsAndKeepsNativeComposer() throws Exception {
        try (ActivityScenario<AIAssistantTestActivity> scenario =
                 ActivityScenario.launch(AIAssistantTestActivity.class)) {
            await(scenario, p -> p.controller.state.ready);
            java.util.concurrent.atomic.AtomicReference<android.widget.EditText> input =
                new java.util.concurrent.atomic.AtomicReference<>();
            scenario.onActivity(a -> {
                input.set(a.panel.composer);
                a.panel.composer.setText("中文🙂 UI streaming check");
                a.panel.send.performClick();
            });
            await(scenario, p -> !p.controller.state.busy() && visibleText(p, "Mobile response ready"));
            scenario.onActivity(a -> {
                assertSame(input.get(), a.panel.composer);
                assertEquals("", a.panel.composer.getText().toString());
            });
            await(scenario, p -> {
                org.json.JSONArray messages = p.controller.state.data.getJSONArray("messages");
                return messages.getJSONObject(0)
                    .getJSONArray("parts")
                    .getJSONObject(0)
                    .getString("text")
                    .equals("中文🙂 UI streaming check");
            });
            scenario.onActivity(a -> {
                a.panel.composer.setText("write");
                a.panel.send.performClick();
            });
            await(scenario,
                p
                -> p.controller.state.data.optJSONArray("permissions") != null
                    && p.controller.state.data.optJSONArray("permissions").length() == 1);
            scenario.onActivity(a -> {
                View allow = findDescription(a.panel, "允许一次");
                assertNotNull(allow);
                assertTrue(allow.performClick());
            });
            await(scenario,
                p
                -> !p.controller.state.busy()
                    && p.controller.state.data.optJSONArray("permissions").length() == 0);
            scenario.onActivity(a -> {
                a.panel.composer.setText("stop");
                a.panel.send.performClick();
            });
            await(scenario, p -> p.controller.state.busy());
            scenario.onActivity(a -> {
                assertEquals("停止", a.panel.send.getContentDescription());
                a.panel.send.performClick();
            });
            await(scenario, p -> !p.controller.state.busy());
            scenario.onActivity(a -> {
                a.panel.composer.requestFocus();
                ((android.view.inputmethod.InputMethodManager) a.getSystemService(
                     Context.INPUT_METHOD_SERVICE))
                    .showSoftInput(
                        a.panel.composer, android.view.inputmethod.InputMethodManager.SHOW_IMPLICIT);
            });
            await(scenario, p -> {
                androidx.core.view.WindowInsetsCompat insets =
                    androidx.core.view.ViewCompat.getRootWindowInsets(p);
                return insets != null && insets.isVisible(androidx.core.view.WindowInsetsCompat.Type.ime());
            });
            scenario.onActivity(a -> {
                a.panel.composer.setText("long");
                a.panel.send.performClick();
            });
            await(scenario, p -> !p.controller.state.busy() && visibleText(p, "Tail marker"));
            await(scenario, p -> {
                android.widget.ListView list = (android.widget.ListView) findList(p);
                if (list == null || list.getLastVisiblePosition() != list.getCount() - 1)
                    return false;
                View last = list.getChildAt(list.getChildCount() - 1);
                androidx.core.view.WindowInsetsCompat insets =
                    androidx.core.view.ViewCompat.getRootWindowInsets(p);
                return last.getBottom() <= list.getHeight() - list.getPaddingBottom() + 2
                    && last.getBottom() > 0 && insets != null
                    && insets.isVisible(androidx.core.view.WindowInsetsCompat.Type.ime());
            });
            CountDownLatch saved = new CountDownLatch(1);
            AtomicBoolean savedOK = new AtomicBoolean();
            scenario.onActivity(a
                -> a.panel.controller.save("https://open.bigmodel.cn/api/coding/paas/v4", "glm-5.3",
                    "on-request", "test-keystore-value", ok -> {
                        savedOK.set(ok);
                        saved.countDown();
                    }));
            assertTrue(saved.await(8, TimeUnit.SECONDS));
            assertTrue(savedOK.get());
            Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
            boolean checked = false;
            for (File directory : context.getNoBackupFilesDir().listFiles())
                if (directory.getName().startsWith("AIAssistantTest-")) {
                    File key = new File(directory, "api-key.enc");
                    if (key.exists()) {
                        String encrypted =
                            new String(Files.readAllBytes(key.toPath()), StandardCharsets.UTF_8);
                        assertFalse(encrypted.contains("test-keystore-value"));
                        checked = true;
                    }
                }
            assertTrue("Encrypted API key file not found", checked);
            Bitmap screenshot =
                InstrumentationRegistry.getInstrumentation().getUiAutomation().takeScreenshot();
            File captures = new File(context.getExternalFilesDir(null), "test-captures");
            captures.mkdirs();
            try (
                FileOutputStream out = new FileOutputStream(new File(captures, "android-ai-assistant.png"))) {
                screenshot.compress(Bitmap.CompressFormat.PNG, 100, out);
            }
        }
    }
    @Test
    public void httpsUsesSystemRootsAndRejectsExpiredCertificate() throws Exception {
        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        checkHttps(context, "https://open.bigmodel.cn/api/coding/paas/v4", false);
        checkHttps(context, "https://expired.badssl.com", true);
    }
    private void checkHttps(Context context, String endpoint, boolean expired) throws Exception {
        AIAssistantController controller = new AIAssistantController(context, endpoint);
        try {
            long deadline = System.currentTimeMillis() + 12000;
            while (!controller.state.ready && System.currentTimeMillis() < deadline) Thread.sleep(50);
            assertTrue(controller.state.error, controller.state.ready);
            controller.setListener(s -> {});
            CountDownLatch submitted = new CountDownLatch(1);
            AtomicBoolean sent = new AtomicBoolean();
            controller.send("TLS integration test", ok -> {
                sent.set(ok);
                submitted.countDown();
            });
            assertTrue(submitted.await(5, TimeUnit.SECONDS));
            assertTrue(sent.get());
            deadline = System.currentTimeMillis() + 30000;
            while (controller.state.error.isEmpty() && System.currentTimeMillis() < deadline)
                Thread.sleep(100);
            String error = controller.state.error;
            if (expired)
                assertTrue(error, error.contains("certificate") || error.contains("SSL"));
            else
                assertTrue(error, error.startsWith("HTTP "));
        } finally {
            controller.close();
        }
    }
}
