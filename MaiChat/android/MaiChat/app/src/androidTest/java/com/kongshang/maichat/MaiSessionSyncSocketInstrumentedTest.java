// Runs the shared C++ WSS transport through the Android JNI bridge on an emulator.
// A local test account and one-day local TLS certificate are supplied by the fixture.
package com.kongshang.maichat;

import static org.junit.Assert.assertTrue;

import android.content.Context;
import android.os.Bundle;
import android.util.Base64;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.nio.charset.StandardCharsets;
import org.junit.Assume;
import org.junit.Test;
import org.junit.runner.RunWith;

@RunWith(AndroidJUnit4.class)
public class MaiSessionSyncSocketInstrumentedTest {
    @Test public void sharedTransportCompletesAuthenticatedTlsHandshake() throws Exception {
        Bundle arguments = InstrumentationRegistry.getArguments();
        String token = arguments.getString("syncToken", "");
        String certificateBase64 = arguments.getString("syncCertificate", "");
        Assume.assumeTrue("Local WSS fixture credentials are required",
            !token.isEmpty() && !certificateBase64.isEmpty());
        String certificate = new String(Base64.decode(certificateBase64, Base64.DEFAULT),
            StandardCharsets.UTF_8);
        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
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
                "wss://10.0.2.2:18191/v1/session-sync/ws", token, certificate, ""));
            long deadline = System.currentTimeMillis() + 10000;
            while (System.currentTimeMillis() < deadline &&
                   !connectedField.getBoolean(coordinator)) Thread.sleep(100);
            assertTrue("Android JNI did not receive the TLS WebSocket open event",
                connectedField.getBoolean(coordinator));
        } finally { coordinator.close(); }
    }
}
