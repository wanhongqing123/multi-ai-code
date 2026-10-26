// 文件作用：Android 拨号工具的无真实通话回归测试。
// 运行边界：使用假系统宿主验证权限复用与拨号 Intent，不发起实际电话。
package com.kongshang.maichat.tools;

import static org.junit.Assert.assertEquals;

import android.content.Intent;
import java.util.ArrayList;
import java.util.List;
import org.json.JSONObject;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;

@RunWith(RobolectricTestRunner.class)
public final class MobileDialPhoneTest {
    @Test public void grantedPermissionIsReusedWithoutAnotherPrompt() throws Exception {
        FakePhoneCallHost host = new FakePhoneCallHost();
        host.grantOnRequest = true;
        MobileTelephonyTools tools = new MobileTelephonyTools(host);

        JSONObject first = tools.dial(new JSONObject().put("phone_number", "+86 (138) 0013-8000"));
        JSONObject second = tools.dial(new JSONObject().put("phone_number", "13800138001"));

        assertEquals("call_requested", first.getString("status"));
        assertEquals("call_requested", second.getString("status"));
        assertEquals(1, host.permissionRequests);
        assertEquals(2, host.calls.size());
        assertEquals(Intent.ACTION_CALL, host.calls.get(0).getAction());
        assertEquals("tel", host.calls.get(0).getData().getScheme());
        assertEquals("+8613800138000", host.calls.get(0).getData().getSchemeSpecificPart());
        assertEquals("13800138001", host.calls.get(1).getData().getSchemeSpecificPart());
    }

    @Test public void deniedPermissionAndInvalidNumberNeverStartCall() throws Exception {
        FakePhoneCallHost host = new FakePhoneCallHost();
        MobileTelephonyTools tools = new MobileTelephonyTools(host);

        JSONObject denied = tools.dial(new JSONObject().put("phone_number", "13800138000"));
        assertEquals("permission_denied", denied.getString("code"));
        assertEquals("phone_call", denied.getString("permission"));
        assertEquals("invalid_input", tools.dial(new JSONObject()
            .put("phone_number", "*21*13800138000#")).getString("code"));
        assertEquals(1, host.permissionRequests);
        assertEquals(0, host.calls.size());
    }

    @Test public void backgroundAppCannotPromptOrStartCall() throws Exception {
        FakePhoneCallHost host = new FakePhoneCallHost();
        host.foreground = false;
        MobileTelephonyTools tools = new MobileTelephonyTools(host);

        assertEquals("unavailable", tools.dial(new JSONObject()
            .put("phone_number", "13800138000")).getString("code"));
        assertEquals(0, host.permissionRequests);
        assertEquals(0, host.calls.size());
    }

    private static final class FakePhoneCallHost implements MobileTelephonyTools.PhoneCallHost {
        boolean foreground = true;
        boolean granted;
        boolean grantOnRequest;
        int permissionRequests;
        final List<Intent> calls = new ArrayList<>();

        @Override public boolean isForeground() { return foreground; }
        @Override public boolean hasTelephony() { return true; }
        @Override public boolean hasCallPermission() { return granted; }
        @Override public boolean requestCallPermission() {
            permissionRequests++;
            granted = grantOnRequest;
            return granted;
        }
        @Override public void startCall(Intent intent) { calls.add(intent); }
    }
}
