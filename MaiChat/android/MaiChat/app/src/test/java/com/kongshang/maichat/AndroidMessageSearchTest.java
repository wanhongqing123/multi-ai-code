package com.kongshang.maichat;

import static org.junit.Assert.assertEquals;

import android.content.Context;

import androidx.test.core.app.ApplicationProvider;

import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;

import java.util.List;

@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34)
public class AndroidMessageSearchTest {
    @Test
    public void searchesCompleteAccountScopedHistoryAndTreatsWildcardsLiterally() {
        Context context = ApplicationProvider.getApplicationContext();
        String databaseName = "maichat-message-search-robolectric.db";
        context.deleteDatabase(databaseName);
        AndroidChatHistoryStore store = new AndroidChatHistoryStore(context, databaseName);

        store.upsertMessage("owner-1", message("old", "peer-1", "owner-1", "Alpha%_旧消息", 100));
        store.upsertMessage("owner-1", message("new", "peer-2", "owner-1", "alphaXX新消息", 200));
        store.upsertMessage("owner-2", message("other", "peer-3", "owner-2", "Alpha%_别的账号", 300));

        List<RemoteIMMessageSearchHit> literal = store.searchMessages("owner-1", "alpha%_", 20);
        assertEquals(1, literal.size());
        assertEquals("peer-1", literal.get(0).peerUserId());
        assertEquals("old", literal.get(0).message().id());

        List<RemoteIMMessageSearchHit> caseInsensitive =
            store.searchMessages("owner-1", "旧消息", 20);
        assertEquals(1, caseInsensitive.size());
        assertEquals("old", caseInsensitive.get(0).message().id());

        RemoteIMMessage quoted = message("quoted", "peer-1", "owner-1", "带引用的消息", 400);
        RemoteIMQuote quote = new RemoteIMQuote("remote-old", "peer-1", "Alpha%_旧消息", "text");
        quoted.setQuote(quote);
        store.upsertMessage("owner-1", quoted);
        RemoteIMMessage restored = store.messageWithRemoteId("owner-1", "peer-1", "remote-quoted");
        assertEquals(quote, restored.quote());

        store.close();
        context.deleteDatabase(databaseName);
    }

    private static RemoteIMMessage message(
        String id,
        String from,
        String to,
        String text,
        long createdAt
    ) {
        return new RemoteIMMessage(
            id,
            "remote-" + id,
            from,
            to,
            text,
            RemoteIMMessage.Direction.INCOMING,
            RemoteIMMessage.Status.RECEIVED,
            createdAt,
            null,
            null,
            null,
            null,
            RemoteIMOrigin.HUMAN
        );
    }
}
