// 文件作用：验证安全表单状态不冒称成功、账号隔离以及在途不可撤回；不访问网络或真实账号。
package com.kongshang.maichat;

import static org.junit.Assert.*;
import org.json.JSONObject;
import org.junit.Test;

public class AccountPasswordRequestsTest {
    @Test public void presentationAndServerCompletionAreDifferentStates() throws Exception {
        MaiAccountPasswordRequests requests = new MaiAccountPasswordRequests();
        String id = requests.open("account-a|1");
        JSONObject value = requests.status(id, "account-a|1");
        assertEquals("opening", value.getString("status"));
        assertFalse(value.getBoolean("opened"));
        assertFalse(value.getBoolean("changed"));
        requests.visible(id, "account-a|1");
        assertEquals("awaiting_user", requests.status(id, "account-a|1").getString("status"));
        requests.submitting(id, "account-a|1");
        requests.dismiss(id);
        assertEquals("submitting", requests.status(id, "account-a|1").getString("status"));
        assertEquals(id, requests.open("account-a|1"));
        requests.finished(id, "account-a|1", null);
        value = requests.status(id, "account-a|1");
        assertTrue(value.getBoolean("changed"));
        assertTrue(value.getBoolean("opened"));
        requests.dismiss(id);
        assertNotEquals(id, requests.open("account-a|1"));
    }

    @Test public void cancellationAndExpiredLoginNeverClaimChanged() throws Exception {
        MaiAccountPasswordRequests requests = new MaiAccountPasswordRequests();
        String first = requests.open("a|1");
        requests.visible(first, "a|1");
        requests.dismiss(first);
        assertEquals("canceled", requests.status(first, "a|1").getString("status"));
        assertFalse(requests.status(first, "a|1").getBoolean("changed"));
        String next = requests.open("a|1");
        requests.visible(next, "a|1");
        requests.submitting(next, "a|1");
        requests.finished(next, "a|1", "reauth_required");
        assertEquals("reauth_required", requests.status(next, "a|1").getString("status"));
        assertFalse(requests.status(next, "a|1").getBoolean("changed"));
        requests.submitting(next, "a|1");
        requests.finished(next, "a|1", "failed");
        assertEquals("failed", requests.status(next, "a|1").getString("status"));
    }

    @Test public void accountChangeCannotInspectOldRequestAndInflightIsNotCanceled() throws Exception {
        MaiAccountPasswordRequests requests = new MaiAccountPasswordRequests();
        String id = requests.open("a|1");
        requests.visible(id, "a|1");
        requests.submitting(id, "a|1");
        requests.open("b|2");
        assertEquals("failed", requests.status(id, "a|1").getString("status"));
        assertEquals("identity_changed", requests.status(id, "a|1").getString("error_code"));
        try { requests.status(id, "b|2"); fail("another account read a password request"); }
        catch (IllegalArgumentException expected) {}
        try { requests.status(id, "a|3"); fail("another login generation reused a password request"); }
        catch (IllegalArgumentException expected) {}
    }

    @Test public void completionRequiresVisibleSubmittingForm() throws Exception {
        MaiAccountPasswordRequests requests = new MaiAccountPasswordRequests();
        String id = requests.open("a|1");
        try { requests.finished(id, "a|1", null); fail("opening claimed changed"); }
        catch (IllegalStateException expected) {}
        requests.visible(id, "a|1");
        try { requests.finished(id, "a|1", null); fail("awaiting input claimed changed"); }
        catch (IllegalStateException expected) {}
    }

    @Test public void passwordsUseServerCodepointAndUtf8BoundsWithoutNormalization() {
        assertFalse(MaiAccountPasswordRequests.isValidPassword("short", "short"));
        String valid = "a safe long passphrase";
        assertTrue(MaiAccountPasswordRequests.isValidPassword(valid, valid));
        assertFalse(MaiAccountPasswordRequests.isValidPassword(valid, valid + " "));
        String emoji = new String(Character.toChars(0x1f642));
        String fifteen = String.join("", java.util.Collections.nCopies(15, emoji));
        assertTrue(MaiAccountPasswordRequests.isValidPassword(fifteen, fifteen));
        String maximum = String.join("", java.util.Collections.nCopies(128, emoji));
        assertTrue(MaiAccountPasswordRequests.isValidPassword(maximum, maximum));
        String excessive = maximum + "a";
        assertFalse(MaiAccountPasswordRequests.isValidPassword(excessive, excessive));
    }
}
