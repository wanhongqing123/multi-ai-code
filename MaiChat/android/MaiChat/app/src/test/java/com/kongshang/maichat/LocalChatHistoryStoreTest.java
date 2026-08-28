package com.kongshang.maichat;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;

import org.junit.Test;

import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Base64;
import java.util.List;

public class LocalChatHistoryStoreTest {
    @Test
    public void contactGroupsAndAssignmentsSurviveRestart() throws Exception {
        Path root = Files.createTempDirectory("maichat-android-groups");
        LocalChatHistoryStore store = new LocalChatHistoryStore(root.toFile());
        ChatState state = new ChatState("android-user");
        state.addContactGroup("同事");
        state.addContactGroup("未分组");
        state.upsertContact(new RemoteIMContact("mac-office", "Mac"));
        state.setContactGroup("mac-office", "同事");

        store.save(state);
        ChatState restored = store.load("android-user");

        assertEquals(List.of("同事", "未分组"), restored.contactGroups());
        assertEquals("同事", restored.contacts().get(0).groupName());
    }

    @Test
    public void savesAndRestoresContactsAndMessages() throws Exception {
        Path root = Files.createTempDirectory("maichat-android-history");
        LocalChatHistoryStore store = new LocalChatHistoryStore(root.toFile());
        ChatState state = new ChatState("android-user");
        state.upsertContact(new RemoteIMContact("mac-office", "Mac Office"));
        state.selectPeer("mac-office");
        RemoteIMMessage text = state.queueOutgoingText("检查构建");
        state.updateMessageStatus(text.id(), RemoteIMMessage.Status.SENT);
        state.queueOutgoingImage("/tmp/photo.png", 640, 480, 4096);
        state.receiveVoice("/tmp/reply.m4a", 4, "mac-office");
        state.receiveFile("/tmp/report.html", "mac-office", "report.html", "text/html", 2048);
        state.receiveVideo(
            "/tmp/clip.mp4",
            "/tmp/clip-cover.jpg",
            12,
            1080,
            1920,
            4_194_304L,
            "mac-office",
            "sdk-video-1",
            // messagesWith 按 createdAtMillis 排序，其余消息用的是当前时间；
            // 这里必须比它们晚，否则视频会插到最前面把索引全打乱。
            System.currentTimeMillis() + 1_000L,
            RemoteIMOrigin.HUMAN
        );

        store.save(state);

        ChatState restored = store.load("android-user");
        assertEquals(state.contacts(), restored.contacts());
        assertEquals(5, restored.messagesWith("mac-office").size());
        assertEquals(RemoteIMMessage.Status.SENT, restored.messagesWith("mac-office").get(0).status());
        assertNotNull(restored.messagesWith("mac-office").get(1).imageAttachment());
        assertNotNull(restored.messagesWith("mac-office").get(2).voiceAttachment());
        assertNotNull(restored.messagesWith("mac-office").get(3).fileAttachment());
        assertEquals("report.html", restored.messagesWith("mac-office").get(3).fileAttachment().fileName());

        RemoteIMVideoAttachment video = restored.messagesWith("mac-office").get(4).videoAttachment();
        assertNotNull(video);
        assertEquals("/tmp/clip.mp4", video.localPath());
        assertEquals("/tmp/clip-cover.jpg", video.coverPath());
        assertEquals(12, video.durationSeconds());
        assertEquals(1080, video.width());
        assertEquals(1920, video.height());
        assertEquals(4_194_304L, video.sizeBytes());
    }

    @Test
    public void readsHistoryWrittenBeforeVideoColumnsExisted() throws Exception {
        // 老版本写下的行没有视频那几列。加列不能让用户已有的历史读不出来，
        // 所以这里直接喂一条老格式的行（18 列），断言仍能正常还原。
        Path root = Files.createTempDirectory("maichat-android-history-legacy");
        Files.createDirectories(root);
        Path file = root.resolve("android-user.tsv");

        String legacyLine = String.join("	",
            "MESSAGE",
            encode("msg-1"),
            encode("mac-office"),
            encode("android-user"),
            encode("旧消息"),
            "INCOMING",
            "RECEIVED",
            "1700000000000",
            encode(""), "0", "0", "0",
            encode(""), "0",
            encode(""), encode(""), encode(""), "0"
        );
        Files.write(file, List.of(legacyLine));

        ChatState restored = new LocalChatHistoryStore(root.toFile()).load("android-user");

        List<RemoteIMMessage> messages = restored.messagesWith("mac-office");
        assertEquals(1, messages.size());
        assertEquals("旧消息", messages.get(0).text());
        assertNull(messages.get(0).videoAttachment());
    }

    @Test
    public void approvalRequestAndDecisionSurviveRestart() throws Exception {
        Path root = Files.createTempDirectory("maichat-android-history-approval");
        LocalChatHistoryStore store = new LocalChatHistoryStore(root.toFile());
        ChatState state = new ChatState("android-user");
        state.upsertContact(new RemoteIMContact("desktop-bot", "Desktop Bot"));
        RemoteIMApprovalRequest request = new RemoteIMApprovalRequest(
            "approval-history-1",
            List.of(RemoteIMApprovalAction.APPROVE_ONCE, RemoteIMApprovalAction.REJECT)
        );
        state.receiveText(
            "需要审批",
            "desktop-bot",
            "remote-approval-1",
            1_700_000_000_000L,
            RemoteIMOrigin.MACHINE,
            request
        );
        RemoteIMMessage decision = state.queueOutgoingApprovalDecision(
            "desktop-bot",
            request.token(),
            RemoteIMApprovalAction.APPROVE_ONCE
        );
        state.updateMessageStatus(decision.id(), RemoteIMMessage.Status.SENT);
        state.receiveText(
            "该审批已自动拒绝",
            "desktop-bot",
            "remote-resolution-1",
            decision.createdAtMillis() + 1_000L,
            RemoteIMOrigin.MACHINE,
            null,
            new RemoteIMApprovalDecision(
                request.token(),
                RemoteIMApprovalAction.AUTO_DECLINED
            )
        );
        store.save(state);

        ChatState restored = store.load("android-user");
        List<RemoteIMMessage> messages = restored.messagesWith("desktop-bot");

        assertEquals(3, messages.size());
        assertEquals(request, messages.get(0).approvalRequest());
        assertEquals(
            new RemoteIMApprovalDecision(
                request.token(),
                RemoteIMApprovalAction.APPROVE_ONCE
            ),
            messages.get(1).approvalDecision()
        );
        assertEquals(
            new RemoteIMApprovalDecision(
                request.token(),
                RemoteIMApprovalAction.AUTO_DECLINED
            ),
            messages.get(2).approvalDecision()
        );
        assertEquals(
            RemoteIMApprovalDisplayPolicy.State.AUTO_DECLINED,
            RemoteIMApprovalDisplayPolicy.stateFor(
                request,
                RemoteIMApprovalDisplayPolicy.statesFor(messages)
            )
        );
    }

    private static String encode(String value) {
        return Base64.getUrlEncoder().encodeToString(value.getBytes(StandardCharsets.UTF_8));
    }

    @Test
    public void returnsEmptyStateWhenHistoryDoesNotExist() throws Exception {
        Path root = Files.createTempDirectory("maichat-android-history-empty");
        LocalChatHistoryStore store = new LocalChatHistoryStore(root.toFile());

        ChatState restored = store.load("android-user");

        assertEquals(List.of(), restored.contacts());
        assertEquals(List.of(), restored.messages());
    }
}
