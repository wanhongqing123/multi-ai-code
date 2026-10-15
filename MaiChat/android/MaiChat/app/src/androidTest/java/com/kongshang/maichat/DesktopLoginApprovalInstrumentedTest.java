// 文件作用：用本机假账号服务实测 Android 扫码后的核对、批准与拒绝页面。
package com.kongshang.maichat;

import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertTrue;
import static org.junit.Assume.assumeTrue;

import android.app.Instrumentation;
import android.content.Context;
import android.content.Intent;
import android.graphics.Bitmap;
import android.os.Bundle;
import android.view.View;
import android.view.ViewGroup;
import android.widget.TextView;

import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;

import org.junit.Test;
import org.junit.runner.RunWith;

import java.io.File;
import java.io.FileOutputStream;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicReference;

@RunWith(AndroidJUnit4.class)
public final class DesktopLoginApprovalInstrumentedTest {
    @Test public void localRequestCanBeApprovedOrDenied() throws Exception {
        assumeTrue("Only the explicit local auth build can run this flow",
            BuildConfig.DEBUG && BuildConfig.USE_LOCAL_UI_AUTH);
        Bundle arguments = InstrumentationRegistry.getArguments();
        String id = arguments.getString("request_id", "");
        String displayCode = arguments.getString("display_code", "");
        String decision = arguments.getString("decision", "");
        assumeTrue("A local QR request must be supplied", id.matches("[0-9a-f-]{36}")
            && displayCode.matches("[0-9]{6}")
            && (decision.equals("approve") || decision.equals("deny")));

        Instrumentation instrumentation = InstrumentationRegistry.getInstrumentation();
        Context context = instrumentation.getTargetContext();
        Intent intent = new Intent(context, DesktopLoginApprovalActivity.class);
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        DesktopLoginApprovalActivity activity =
            (DesktopLoginApprovalActivity) instrumentation.startActivitySync(intent);
        try {
            AtomicBoolean accepted = new AtomicBoolean();
            instrumentation.runOnMainSync(() -> accepted.set(activity.acceptQrUri(
                "maichat://auth/desktop?request_id=" + id)));
            assertTrue(accepted.get());
            assertNotNull(waitForText(instrumentation, activity, displayCode, 15_000));
            capture(context, instrumentation, "confirm-" + decision);

            String button = decision.equals("approve")
                ? "数字一致，批准登录" : "拒绝登录";
            View action = waitForText(instrumentation, activity, button, 2_000);
            assertNotNull(action);
            instrumentation.runOnMainSync(action::performClick);
            String result = decision.equals("approve")
                ? "已批准桌面登录" : "已拒绝桌面登录";
            assertNotNull(waitForText(instrumentation, activity, result, 15_000));
            capture(context, instrumentation, "result-" + decision);
        } finally {
            instrumentation.runOnMainSync(activity::finish);
        }
    }

    private static View waitForText(
        Instrumentation instrumentation, DesktopLoginApprovalActivity activity,
        String value, long timeoutMillis) throws Exception {
        long deadline = System.currentTimeMillis() + timeoutMillis;
        while (System.currentTimeMillis() < deadline) {
            AtomicReference<View> found = new AtomicReference<>();
            instrumentation.runOnMainSync(() -> found.set(findText(
                activity.getWindow().getDecorView(), value)));
            if (found.get() != null) return found.get();
            Thread.sleep(100);
        }
        return null;
    }

    private static View findText(View view, String value) {
        if (view instanceof TextView && value.contentEquals(((TextView) view).getText()))
            return view;
        if (view instanceof ViewGroup) {
            ViewGroup group = (ViewGroup) view;
            for (int index = 0; index < group.getChildCount(); index++) {
                View found = findText(group.getChildAt(index), value);
                if (found != null) return found;
            }
        }
        return null;
    }

    private static void capture(
        Context context, Instrumentation instrumentation, String name) throws Exception {
        instrumentation.waitForIdleSync();
        Thread.sleep(500);
        Bitmap bitmap = instrumentation.getUiAutomation().takeScreenshot();
        assertNotNull(bitmap);
        File directory = new File(context.getExternalFilesDir(null), "desktop-login-captures");
        assertTrue(directory.mkdirs() || directory.isDirectory());
        try (FileOutputStream output = new FileOutputStream(new File(directory, name + ".png"))) {
            assertTrue(bitmap.compress(Bitmap.CompressFormat.PNG, 100, output));
        }
        bitmap.recycle();
    }
}
