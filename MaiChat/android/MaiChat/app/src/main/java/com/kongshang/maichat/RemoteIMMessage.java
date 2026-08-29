package com.kongshang.maichat;

import java.util.Objects;
import java.util.UUID;

public final class RemoteIMMessage {
    public enum Direction {
        INCOMING,
        OUTGOING
    }

    public enum Status {
        PENDING,
        SENT,
        RECEIVED,
        FAILED
    }

    private final String id;
    private String remoteId;
    private final String fromUserId;
    private final String toUserId;
    private final String text;
    private final Direction direction;
    private Status status;
    private final long createdAtMillis;
    private final RemoteIMImageAttachment imageAttachment;
    private final RemoteIMVoiceAttachment voiceAttachment;
    private final RemoteIMFileAttachment fileAttachment;
    private final RemoteIMVideoAttachment videoAttachment;
    private final RemoteIMOrigin origin;
    private final RemoteIMApprovalRequest approvalRequest;
    private final RemoteIMApprovalDecision approvalDecision;
    private boolean captionAbove;

    public RemoteIMMessage(
        String fromUserId,
        String toUserId,
        String text,
        Direction direction,
        Status status,
        long createdAtMillis,
        RemoteIMImageAttachment imageAttachment,
        RemoteIMVoiceAttachment voiceAttachment
    ) {
        this(
            UUID.randomUUID().toString(),
            null,
            fromUserId,
            toUserId,
            text,
            direction,
            status,
            createdAtMillis,
            imageAttachment,
            voiceAttachment,
            null,
            null,
            RemoteIMOrigin.HUMAN
        );
    }

    public RemoteIMMessage(
        String fromUserId,
        String toUserId,
        String text,
        Direction direction,
        Status status,
        long createdAtMillis,
        RemoteIMImageAttachment imageAttachment,
        RemoteIMVoiceAttachment voiceAttachment,
        RemoteIMFileAttachment fileAttachment
    ) {
        this(
            UUID.randomUUID().toString(),
            null,
            fromUserId,
            toUserId,
            text,
            direction,
            status,
            createdAtMillis,
            imageAttachment,
            voiceAttachment,
            fileAttachment,
            null,
            RemoteIMOrigin.HUMAN
        );
    }

    RemoteIMMessage(
        String id,
        String fromUserId,
        String toUserId,
        String text,
        Direction direction,
        Status status,
        long createdAtMillis,
        RemoteIMImageAttachment imageAttachment,
        RemoteIMVoiceAttachment voiceAttachment
    ) {
        this(
            id,
            null,
            fromUserId,
            toUserId,
            text,
            direction,
            status,
            createdAtMillis,
            imageAttachment,
            voiceAttachment,
            null,
            null,
            RemoteIMOrigin.HUMAN
        );
    }

    RemoteIMMessage(
        String id,
        String fromUserId,
        String toUserId,
        String text,
        Direction direction,
        Status status,
        long createdAtMillis,
        RemoteIMImageAttachment imageAttachment,
        RemoteIMVoiceAttachment voiceAttachment,
        RemoteIMFileAttachment fileAttachment
    ) {
        this(
            id,
            null,
            fromUserId,
            toUserId,
            text,
            direction,
            status,
            createdAtMillis,
            imageAttachment,
            voiceAttachment,
            fileAttachment,
            null,
            RemoteIMOrigin.HUMAN
        );
    }

    RemoteIMMessage(
        String id,
        String fromUserId,
        String toUserId,
        String text,
        Direction direction,
        Status status,
        long createdAtMillis,
        RemoteIMImageAttachment imageAttachment,
        RemoteIMVoiceAttachment voiceAttachment,
        RemoteIMFileAttachment fileAttachment,
        RemoteIMVideoAttachment videoAttachment
    ) {
        this(
            id,
            null,
            fromUserId,
            toUserId,
            text,
            direction,
            status,
            createdAtMillis,
            imageAttachment,
            voiceAttachment,
            fileAttachment,
            videoAttachment,
            RemoteIMOrigin.HUMAN
        );
    }

    RemoteIMMessage(
        String id,
        String remoteId,
        String fromUserId,
        String toUserId,
        String text,
        Direction direction,
        Status status,
        long createdAtMillis,
        RemoteIMImageAttachment imageAttachment,
        RemoteIMVoiceAttachment voiceAttachment,
        RemoteIMFileAttachment fileAttachment,
        RemoteIMVideoAttachment videoAttachment,
        RemoteIMOrigin origin
    ) {
        this(
            id,
            remoteId,
            fromUserId,
            toUserId,
            text,
            direction,
            status,
            createdAtMillis,
            imageAttachment,
            voiceAttachment,
            fileAttachment,
            videoAttachment,
            origin,
            null,
            null
        );
    }

    RemoteIMMessage(
        String id,
        String remoteId,
        String fromUserId,
        String toUserId,
        String text,
        Direction direction,
        Status status,
        long createdAtMillis,
        RemoteIMImageAttachment imageAttachment,
        RemoteIMVoiceAttachment voiceAttachment,
        RemoteIMFileAttachment fileAttachment,
        RemoteIMVideoAttachment videoAttachment,
        RemoteIMOrigin origin,
        RemoteIMApprovalRequest approvalRequest,
        RemoteIMApprovalDecision approvalDecision
    ) {
        this.id = clean(id).isEmpty() ? UUID.randomUUID().toString() : clean(id);
        this.remoteId = clean(remoteId);
        this.fromUserId = clean(fromUserId);
        this.toUserId = clean(toUserId);
        this.text = clean(text);
        this.direction = direction;
        this.status = status;
        this.createdAtMillis = createdAtMillis;
        this.imageAttachment = imageAttachment;
        this.voiceAttachment = voiceAttachment;
        this.fileAttachment = fileAttachment;
        this.videoAttachment = videoAttachment;
        this.origin = origin == null ? RemoteIMOrigin.MACHINE : origin;
        this.approvalRequest = approvalRequest;
        this.approvalDecision = approvalDecision;
    }

    public String id() {
        return id;
    }

    public String fromUserId() {
        return fromUserId;
    }

    public String remoteId() {
        return remoteId;
    }

    public void setRemoteId(String remoteId) {
        this.remoteId = clean(remoteId);
    }

    public String toUserId() {
        return toUserId;
    }

    public String text() {
        return text;
    }

    public Direction direction() {
        return direction;
    }

    public Status status() {
        return status;
    }

    public void setStatus(Status status) {
        this.status = status;
    }

    public long createdAtMillis() {
        return createdAtMillis;
    }

    public RemoteIMImageAttachment imageAttachment() {
        return imageAttachment;
    }

    public RemoteIMVoiceAttachment voiceAttachment() {
        return voiceAttachment;
    }

    public RemoteIMVideoAttachment videoAttachment() {
        return videoAttachment;
    }

    public RemoteIMFileAttachment fileAttachment() {
        return fileAttachment;
    }

    public RemoteIMOrigin origin() {
        return origin;
    }

    public RemoteIMApprovalRequest approvalRequest() {
        return approvalRequest;
    }

    public RemoteIMApprovalDecision approvalDecision() {
        return approvalDecision;
    }

    public boolean captionAbove() {
        return captionAbove;
    }

    public void setCaptionAbove(boolean captionAbove) {
        this.captionAbove = captionAbove;
    }

    private static String clean(String value) {
        return value == null ? "" : value.trim();
    }

    @Override
    public boolean equals(Object other) {
        if (this == other) return true;
        if (!(other instanceof RemoteIMMessage)) return false;
        RemoteIMMessage that = (RemoteIMMessage) other;
        return createdAtMillis == that.createdAtMillis
            && id.equals(that.id)
            && remoteId.equals(that.remoteId)
            && fromUserId.equals(that.fromUserId)
            && toUserId.equals(that.toUserId)
            && text.equals(that.text)
            && direction == that.direction
            && status == that.status
            && Objects.equals(imageAttachment, that.imageAttachment)
            && Objects.equals(voiceAttachment, that.voiceAttachment)
            && Objects.equals(fileAttachment, that.fileAttachment)
            && Objects.equals(videoAttachment, that.videoAttachment)
            && captionAbove == that.captionAbove
            && origin == that.origin
            && Objects.equals(approvalRequest, that.approvalRequest)
            && Objects.equals(approvalDecision, that.approvalDecision);
    }

    @Override
    public int hashCode() {
        return Objects.hash(
            id,
            remoteId,
            fromUserId,
            toUserId,
            text,
            direction,
            status,
            createdAtMillis,
            imageAttachment,
            voiceAttachment,
            fileAttachment,
            videoAttachment,
            captionAbove,
            origin,
            approvalRequest,
            approvalDecision
        );
    }
}
