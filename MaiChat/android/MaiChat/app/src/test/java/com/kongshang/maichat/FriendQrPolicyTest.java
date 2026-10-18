package com.kongshang.maichat;

import static org.junit.Assert.assertEquals;

import org.junit.Test;

public class FriendQrPolicyTest {
    @Test public void accountIdRoundTripsWithoutCredentials() {
        String userId = "u0123456789abcdef01234567";
        String link = FriendQrPolicy.link(userId);
        assertEquals("maichat://friend/add?user_id=" + userId, link);
        assertEquals(userId, FriendQrPolicy.userId(link));
        assertEquals("", FriendQrPolicy.userId(link + "&token=secret"));
        assertEquals("", FriendQrPolicy.userId(
            "maichat://auth/desktop?request_id=123e4567-e89b-42d3-a456-426614174000"));
        assertEquals("", FriendQrPolicy.link("invalid id"));
    }
}
