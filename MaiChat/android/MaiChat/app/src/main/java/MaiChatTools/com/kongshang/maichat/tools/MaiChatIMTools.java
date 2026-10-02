package com.kongshang.maichat.tools;

import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.media.MediaMetadataRetriever;
import android.os.Build;
import com.kongshang.maichat.AIAssistantController;
import com.kongshang.maichat.MainActivity;
import com.kongshang.maichat.MessageQuote;
import com.kongshang.maichat.RemoteIMContact;
import com.kongshang.maichat.RemoteIMMessage;
import com.kongshang.maichat.RemoteIMMessageSearchHit;
import com.kongshang.maichat.RemoteIMMediaPaths;
import com.kongshang.maichat.RemoteIMMediaStore;
import com.kongshang.maichat.RemoteIMQuote;
import com.kongshang.maichat.RemoteIMSessionController;
import com.kongshang.maichat.RemoteIMVideoAttachment;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.file.Files;
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
            case "maichat_send_media": return hostSendMedia(arguments);
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

    private JSONObject hostSendMedia(JSONObject arguments) throws Exception {
        String peer = arguments.optString("peer_id", "").trim();
        String kind = arguments.optString("type", "").trim();
        String caption = arguments.optString("caption", "").trim();
        File source = AIAssistantController.shared(activity)
            .workspaceFile(arguments.optString("file_path", ""));
        boolean contactExists = false;
        for (RemoteIMContact contact : session.chatState().contacts())
            if (peer.equals(contact.userId())) contactExists = true;
        if (!contactExists || source == null || !source.isFile())
            throw new IllegalArgumentException("peer_id 和工作区 file_path 必须有效");
        if (!kind.equals("image") && !kind.equals("video") && !kind.equals("audio"))
            throw new IllegalArgumentException("type 只能是 image、video 或 audio");
        if (source.length() <= 0 || source.length() > (kind.equals("image")
            ? 20L * 1024 * 1024 : 1024L * 1024 * 1024))
            throw new IllegalArgumentException("媒体文件为空或超出大小限制");
        RemoteIMMediaStore store = new RemoteIMMediaStore(RemoteIMMediaPaths.forApp(activity));
        File copied = store.createOutgoingFile("agent-" + java.util.UUID.randomUUID()
            + source.getName());
        try {
            Files.copy(source.toPath(), copied.toPath());
        } catch (IOException error) {
            Files.deleteIfExists(copied.toPath());
            throw error;
        }
        RemoteIMMessage queued;
        if (kind.equals("image")) {
            BitmapFactory.Options options = new BitmapFactory.Options();
            options.inJustDecodeBounds = true;
            BitmapFactory.decodeFile(copied.getAbsolutePath(), options);
            if (options.outWidth <= 0 || options.outHeight <= 0)
                throw new IllegalArgumentException("图片无法解码");
            queued = session.sendImageMessageTo(peer, copied.getAbsolutePath(),
                options.outWidth, options.outHeight, copied.length());
        } else if (kind.equals("video")) {
            queued = session.sendVideoMessageTo(peer, prepareAgentVideo(copied, store));
        } else {
            queued = session.sendFileMessageTo(peer, copied.getAbsolutePath(),
                source.getName(), audioMimeType(source.getName()), copied.length());
        }
        boolean captionQueued = true;
        if (!caption.isEmpty()) {
            try { session.sendTextMessageTo(peer, caption, null); }
            catch (Exception error) { captionQueued = false; }
        }
        return new JSONObject().put("queued", true).put("peer_id", peer)
            .put("type", kind).put("message_kind", kind.equals("audio") ? "file" : kind)
            .put("message_id", queued.id()).put("caption_queued", captionQueued);
    }

    private static RemoteIMVideoAttachment prepareAgentVideo(
        File file, RemoteIMMediaStore store
    ) throws IOException {
        MediaMetadataRetriever retriever = new MediaMetadataRetriever();
        Bitmap cover = null;
        try {
            retriever.setDataSource(file.getAbsolutePath());
            int width = mediaNumber(retriever.extractMetadata(
                MediaMetadataRetriever.METADATA_KEY_VIDEO_WIDTH));
            int height = mediaNumber(retriever.extractMetadata(
                MediaMetadataRetriever.METADATA_KEY_VIDEO_HEIGHT));
            int duration = mediaNumber(retriever.extractMetadata(
                MediaMetadataRetriever.METADATA_KEY_DURATION));
            if (width <= 0 || height <= 0 || duration <= 0)
                throw new IOException("视频没有可播放的画面或时长");
            if (Build.VERSION.SDK_INT >= 27)
                cover = retriever.getScaledFrameAtTime(0,
                    MediaMetadataRetriever.OPTION_CLOSEST_SYNC, 480,
                    Math.max(1, Math.min(480, height * 480 / width)));
            if (cover == null)
                cover = retriever.getFrameAtTime(0,
                    MediaMetadataRetriever.OPTION_CLOSEST_SYNC);
            if (cover == null) throw new IOException("无法生成视频封面");
            File poster = store.createOutgoingFile("agent-cover-"
                + java.util.UUID.randomUUID() + ".jpg");
            try (FileOutputStream output = new FileOutputStream(poster)) {
                if (!cover.compress(Bitmap.CompressFormat.JPEG, 85, output))
                    throw new IOException("无法保存视频封面");
            }
            return new RemoteIMVideoAttachment(file.getAbsolutePath(),
                poster.getAbsolutePath(), Math.max(1, (duration + 999) / 1000),
                width, height, file.length());
        } finally {
            if (cover != null) cover.recycle();
            retriever.release();
        }
    }

    private static int mediaNumber(String value) {
        try { return Integer.parseInt(value); }
        catch (NumberFormatException error) { return 0; }
    }

    private static String audioMimeType(String name) {
        String lower = name.toLowerCase(Locale.ROOT);
        if (lower.endsWith(".m4a") || lower.endsWith(".aac")) return "audio/mp4";
        if (lower.endsWith(".wav")) return "audio/wav";
        if (lower.endsWith(".ogg")) return "audio/ogg";
        return "audio/mpeg";
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
