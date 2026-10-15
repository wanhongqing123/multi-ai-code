// 文件作用：Android IM 身份、消息同步与本地历史；RemoteIMMessageSearchHit 负责消息搜索结果的标识与定位信息，本文件给出其实现。
// 主要入口：peerUserId、message、clean。
package com.kongshang.maichat;

import java.util.Objects;

public final class RemoteIMMessageSearchHit {
    private final String peerUserId;
    private final RemoteIMMessage message;

    public RemoteIMMessageSearchHit(String peerUserId, RemoteIMMessage message) {
        this.peerUserId = clean(peerUserId);
        this.message = Objects.requireNonNull(message, "message");
        if (this.peerUserId.isEmpty()) {
            throw new IllegalArgumentException("peer user id is required");
        }
    }

    public String peerUserId() {
        return peerUserId;
    }

    public RemoteIMMessage message() {
        return message;
    }

    private static String clean(String value) {
        return value == null ? "" : value.trim();
    }
}
