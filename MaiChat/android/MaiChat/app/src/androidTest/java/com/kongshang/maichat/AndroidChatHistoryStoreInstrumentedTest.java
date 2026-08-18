package com.kongshang.maichat;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

import android.content.Context;

import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;

import org.junit.Test;
import org.junit.runner.RunWith;

import java.util.List;

@RunWith(AndroidJUnit4.class)
public class AndroidChatHistoryStoreInstrumentedTest {
    @Test
    public void testIncrementalUpsertSummaryPaginationAndConversationDelete() {
        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        context.deleteDatabase("maichat-history.db");
        AndroidChatHistoryStore store = new AndroidChatHistoryStore(context);
        String owner = "android-test-owner";
        String peer = "mac-test-peer";
        store.upsertContact(owner, new RemoteIMContact(peer, "Mac Test"));

        for (int index = 0; index < 55; index += 1) {
            RemoteIMMessage message = new RemoteIMMessage(
                "local-" + index,
                "remote-" + index,
                index % 2 == 0 ? owner : peer,
                index % 2 == 0 ? peer : owner,
                "message-" + index,
                index % 2 == 0
                    ? RemoteIMMessage.Direction.OUTGOING
                    : RemoteIMMessage.Direction.INCOMING,
                index % 2 == 0
                    ? RemoteIMMessage.Status.SENT
                    : RemoteIMMessage.Status.RECEIVED,
                1_000L + index,
                null,
                null,
                null,
                RemoteIMOrigin.HUMAN
            );
            store.upsertMessage(owner, message);
        }

        assertEquals(1, store.loadContacts(owner).size());
        List<RemoteIMMessage> summaries = store.loadConversationSummaries(owner);
        assertEquals(1, summaries.size());
        assertEquals("message-54", summaries.get(0).text());

        AndroidChatHistoryStore.Page first = store.loadConversationPage(
            owner,
            peer,
            null,
            null,
            50
        );
        assertEquals(50, first.messages().size());
        assertTrue(first.hasEarlier());
        RemoteIMMessage oldest = first.messages().get(0);
        AndroidChatHistoryStore.Page second = store.loadConversationPage(
            owner,
            peer,
            oldest.createdAtMillis(),
            oldest.id(),
            50
        );
        assertEquals(5, second.messages().size());
        assertFalse(second.hasEarlier());
        assertTrue(store.containsRemoteId(owner, "remote-54"));

        store.deleteConversation(owner, peer);
        assertTrue(store.loadConversationPage(owner, peer, null, null, 50).messages().isEmpty());
        assertEquals(1, store.loadContacts(owner).size());
        store.close();
    }
}
