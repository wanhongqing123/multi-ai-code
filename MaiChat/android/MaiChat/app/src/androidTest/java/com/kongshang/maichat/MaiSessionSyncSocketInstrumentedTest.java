// Runs the shared C++ WSS transport through the Android JNI bridge on an emulator.
// A local test account and one-day local TLS certificate are supplied by the fixture.
package com.kongshang.maichat;

import static org.junit.Assert.assertTrue;
import static org.junit.Assert.assertEquals;

import android.content.Context;
import android.os.Bundle;
import android.util.Base64;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import java.io.File;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.lang.reflect.Constructor;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.TimeUnit;
import java.util.function.BiConsumer;
import org.json.JSONArray;
import org.json.JSONObject;
import org.junit.Assume;
import org.junit.Test;
import org.junit.runner.RunWith;

@RunWith(AndroidJUnit4.class)
public class MaiSessionSyncSocketInstrumentedTest {
    @Test public void lifecycleReplyCompletesBeforeMailboxConsumerRuns() throws Exception {
        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        AndroidSessionSyncCoordinator coordinator =
            new AndroidSessionSyncCoordinator(context, null, null);
        try {
            Class<?> pendingClass = Class.forName(
                AndroidSessionSyncCoordinator.class.getName() + "$PendingLifecycle");
            Constructor<?> constructor = pendingClass.getDeclaredConstructor(
                String.class, String.class, BiConsumer.class);
            constructor.setAccessible(true);
            CompletableFuture<JSONObject> completed = new CompletableFuture<>();
            Object pending = constructor.newInstance("mac", "owner-login",
                (BiConsumer<JSONObject, String>) (result, error) -> {
                    if (!error.isEmpty()) completed.completeExceptionally(new Exception(error));
                    else completed.complete(result);
                });
            Field field = AndroidSessionSyncCoordinator.class.getDeclaredField("pendingLifecycle");
            field.setAccessible(true);
            @SuppressWarnings("unchecked")
            Map<String, Object> entries = (Map<String, Object>) field.get(coordinator);
            entries.put("command-1", pending);
            JSONObject event = new JSONObject().put("kind", "event")
                .put("session_id", "session_lifecycle")
                .put("from_slot", "mac")
                .put("from_login_session_id", "owner-login")
                .put("payload", new JSONObject().put("type", "command_result")
                    .put("command_id", "command-1")
                    .put("result", new JSONObject().put("ok", true)
                        .put("session_id", "ses_created")));
            JSONObject frame = new JSONObject().put("type", "inbox")
                .put("messages", new JSONArray().put(event));
            Method receive = AndroidSessionSyncCoordinator.class.getDeclaredMethod(
                "onNativeSocketEvent", int.class, byte[].class);
            receive.setAccessible(true);
            receive.invoke(coordinator, 3, frame.toString().getBytes(StandardCharsets.UTF_8));
            assertEquals("ses_created", completed.get(1, TimeUnit.SECONDS)
                .getString("session_id"));
        } finally { coordinator.close(); }
    }

    @Test public void sharedTransportCompletesAuthenticatedTlsHandshake() throws Exception {
        Bundle arguments = InstrumentationRegistry.getArguments();
        String token = arguments.getString("syncToken", "");
        String certificateBase64 = arguments.getString("syncCertificate", "");
        boolean production = "1".equals(arguments.getString("syncProduction", ""));
        Assume.assumeTrue("Local WSS fixture credentials are required",
            !token.isEmpty() && (production || !certificateBase64.isEmpty()));
        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        String certificate;
        if (production) {
            File roots = new File(context.getCacheDir(), "session-sync-test-roots.pem");
            try {
                SystemCertificateBundle.writeCurrentPem(roots);
                certificate = new String(Files.readAllBytes(roots.toPath()), StandardCharsets.UTF_8);
            } finally { roots.delete(); }
        } else {
            certificate = new String(Base64.decode(certificateBase64, Base64.DEFAULT),
                StandardCharsets.UTF_8);
        }
        System.loadLibrary("maichat_agent");
        AndroidSessionSyncCoordinator coordinator =
            new AndroidSessionSyncCoordinator(context, null, null);
        Method create = AndroidSessionSyncCoordinator.class.getDeclaredMethod("nativeCreate");
        Method start = AndroidSessionSyncCoordinator.class.getDeclaredMethod(
            "nativeStart", long.class, String.class, String.class, String.class, String.class);
        Field handleField = AndroidSessionSyncCoordinator.class.getDeclaredField("socketHandle");
        Field connectedField = AndroidSessionSyncCoordinator.class.getDeclaredField("connected");
        create.setAccessible(true);
        start.setAccessible(true);
        handleField.setAccessible(true);
        connectedField.setAccessible(true);
        long handle = (long) create.invoke(coordinator);
        assertTrue(handle != 0);
        handleField.setLong(coordinator, handle);
        try {
            assertTrue((boolean) start.invoke(coordinator, handle,
                production ? "wss://ichat.life/api/v1/session-sync/ws" :
                    "wss://10.0.2.2:18191/v1/session-sync/ws", token, certificate, ""));
            long deadline = System.currentTimeMillis() + 10000;
            while (System.currentTimeMillis() < deadline &&
                   !connectedField.getBoolean(coordinator)) Thread.sleep(100);
            assertTrue("Android JNI did not receive the TLS WebSocket open event",
                connectedField.getBoolean(coordinator));
        } finally { coordinator.close(); }
    }
}
