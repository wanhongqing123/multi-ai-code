package com.kongshang.maichat.tools;

import com.kongshang.maichat.MainActivity;
import com.kongshang.maichat.MessageQuote;
import com.kongshang.maichat.RemoteIMContact;
import com.kongshang.maichat.RemoteIMMessage;
import com.kongshang.maichat.RemoteIMMessageSearchHit;
import com.kongshang.maichat.RemoteIMQuote;
import com.kongshang.maichat.RemoteIMSessionController;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;
import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

final class MaiChatIMTools {
    private final MainActivity activity;
    private final RemoteIMSessionController session;

    MaiChatIMTools(MainActivity activity, RemoteIMSessionController session) {
        this.activity = activity;
        this.session = session;
    }

    Object execute(String tool, JSONObject arguments) throws Exception {
        if (session == null || session.requiresLogin())
            throw new IllegalStateException("MaiChat 尚未登录");
        switch (tool) {
            case "maichat_list_contacts": return hostListContacts(arguments);
            case "maichat_list_conversations": return hostListConversations(arguments);
            case "maichat_get_messages": return hostGetMessages(arguments);
            case "maichat_search_messages": return hostSearchMessages(arguments);
            case "maichat_get_unread_summary": return hostUnreadSummary();
            case "maichat_send_text": return hostSendText(arguments, false);
            case "maichat_reply_message": return hostSendText(arguments, true);
            case "maichat_broadcast_text": return hostBroadcastText(arguments);
            default: throw new IllegalArgumentException("未知 MaiChat 宿主工具：" + tool);
        }
    }

    private JSONObject hostListContacts(JSONObject arguments) throws JSONException {
        String query = arguments.optString("query").trim().toLowerCase(Locale.ROOT);
        int limit = hostToolLimit(arguments);
        JSONArray values = new JSONArray();
        for (RemoteIMContact contact : session.chatState().contacts()) {
            if (!query.isEmpty()
                && !contact.userId().toLowerCase(Locale.ROOT).contains(query)
                && !contact.displayName().toLowerCase(Locale.ROOT).contains(query)) continue;
            values.put(new JSONObject()
                .put("user_id", contact.userId())
                .put("display_name", contact.displayName())
                .put("group", contact.groupName()));
            if (values.length() >= limit) break;
        }
        return new JSONObject().put("contacts", values).put("count", values.length());
    }

    private JSONObject hostListConversations(JSONObject arguments) throws JSONException {
        List<JSONObject> rows = new ArrayList<>();
        for (RemoteIMContact contact : session.chatState().contacts()) {
            RemoteIMMessage latest = activity.latestMessage(contact.userId());
            if (latest == null) continue;
            rows.add(new JSONObject()
                .put("peer_id", contact.userId())
                .put("display_name", contact.displayName())
                .put("unread", session.unreadCount(contact.userId()))
                .put("latest", hostMessageJson(latest, contact.userId())));
        }
        rows.sort((left, right) -> Long.compare(
            right.optJSONObject("latest").optLong("created_at_ms"),
            left.optJSONObject("latest").optLong("created_at_ms")));
        JSONArray values = new JSONArray();
        int limit = hostToolLimit(arguments);
        for (int index = 0; index < rows.size() && index < limit; index += 1)
            values.put(rows.get(index));
        return new JSONObject().put("conversations", values).put("count", values.length());
    }

    private JSONObject hostGetMessages(JSONObject arguments) throws JSONException {
        String peer = arguments.optString("peer_id").trim();
        if (peer.isEmpty()) throw new IllegalArgumentException("peer_id 不能为空");
        List<RemoteIMMessage> all = session.chatState().messagesWith(peer);
        int begin = Math.max(0, all.size() - hostToolLimit(arguments));
        JSONArray values = new JSONArray();
        for (int index = begin; index < all.size(); index += 1)
            values.put(hostMessageJson(all.get(index), peer));
        return new JSONObject().put("peer_id", peer).put("messages", values)
            .put("count", values.length());
    }

    private JSONObject hostSearchMessages(JSONObject arguments) throws JSONException {
        String query = arguments.optString("query").trim();
        String peer = arguments.optString("peer_id").trim();
        if (query.isEmpty()) throw new IllegalArgumentException("query 不能为空");
        JSONArray values = new JSONArray();
        for (RemoteIMMessageSearchHit hit : session.searchMessages(query, hostToolLimit(arguments))) {
            if (!peer.isEmpty() && !peer.equals(hit.peerUserId())) continue;
            values.put(hostMessageJson(hit.message(), hit.peerUserId()));
        }
        return new JSONObject().put("query", query).put("matches", values)
            .put("count", values.length());
    }

    private JSONObject hostUnreadSummary() throws JSONException {
        JSONArray values = new JSONArray();
        for (RemoteIMContact contact : session.chatState().contacts()) {
            int unread = session.unreadCount(contact.userId());
            if (unread <= 0) continue;
            values.put(new JSONObject()
                .put("peer_id", contact.userId())
                .put("display_name", contact.displayName())
                .put("unread", unread));
        }
        return new JSONObject().put("total_unread", session.totalUnreadCount())
            .put("conversations", values);
    }

    private JSONObject hostSendText(JSONObject arguments, boolean reply) throws Exception {
        String peer = arguments.optString("peer_id").trim();
        String text = arguments.optString("text").trim();
        if (peer.isEmpty() || text.isEmpty())
            throw new IllegalArgumentException("peer_id 和 text 不能为空");
        boolean contactExists = false;
        for (RemoteIMContact contact : session.chatState().contacts())
            if (peer.equals(contact.userId())) contactExists = true;
        if (!contactExists) throw new IllegalArgumentException("peer_id 不是 MaiChat 好友");
        RemoteIMQuote quote = null;
        if (reply) {
            String messageId = arguments.optString("message_id").trim();
            if (messageId.isEmpty()) throw new IllegalArgumentException("message_id 不能为空");
            RemoteIMMessage source = null;
            for (RemoteIMMessage message : session.chatState().messagesWith(peer))
                if (messageId.equals(message.id())) source = message;
            if (source == null) throw new IllegalArgumentException("找不到要回复的消息");
            quote = MessageQuote.from(source);
        }
        RemoteIMMessage queued = session.sendTextMessageTo(peer, text, quote);
        return new JSONObject().put("sent", true).put("peer_id", peer)
            .put("message_id", queued.id());
    }

    private JSONObject hostBroadcastText(JSONObject arguments) throws Exception {
        JSONArray values = arguments.optJSONArray("peer_ids");
        String text = arguments.optString("text").trim();
        if (values == null || values.length() == 0 || values.length() > 200 || text.isEmpty())
            throw new IllegalArgumentException("peer_ids 和 text 不能为空，收件人最多 200 个");

        Set<String> contacts = new HashSet<>();
        for (RemoteIMContact contact : session.chatState().contacts())
            contacts.add(contact.userId());
        Set<String> seen = new HashSet<>();
        List<String> recipients = new ArrayList<>();
        for (int index = 0; index < values.length(); index += 1) {
            String peer = values.optString(index).trim();
            if (peer.isEmpty() || !seen.add(peer)) continue;
            if (!contacts.contains(peer))
                throw new IllegalArgumentException("peer_ids 包含非好友账号：" + peer);
            recipients.add(peer);
        }
        if (recipients.isEmpty()) throw new IllegalArgumentException("peer_ids 不能为空");

        int queued = session.broadcastText(recipients, text, null);
        if (queued != recipients.size())
            throw new IllegalStateException("MaiChat 未能排队全部群发消息");
        return new JSONObject().put("queued", true).put("recipient_count", queued)
            .put("peer_ids", new JSONArray(recipients));
    }

    private JSONObject hostMessageJson(RemoteIMMessage message, String peer) throws JSONException {
        return new JSONObject()
            .put("id", message.id())
            .put("peer_id", peer)
            .put("direction", message.direction() == RemoteIMMessage.Direction.OUTGOING
                ? "outgoing" : "incoming")
            .put("sender_id", message.fromUserId())
            .put("text", message.text())
            .put("kind", MessageQuote.kind(message))
            .put("created_at_ms", message.createdAtMillis());
    }

    private static int hostToolLimit(JSONObject arguments) {
        return Math.max(1, Math.min(200, arguments.optInt("limit", 50)));
    }
}
