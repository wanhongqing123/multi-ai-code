// 文件作用：承载好友申请列表显示所需的公开 IM 资料，不包含账号登录凭据。
package com.kongshang.maichat;

final class RemoteIMFriendRequest {
    final String userId;
    final String displayName;
    final String avatarUrl;
    final String message;

    RemoteIMFriendRequest(String userId, String displayName, String avatarUrl, String message) {
        this.userId = userId;
        this.displayName = displayName;
        this.avatarUrl = avatarUrl;
        this.message = message;
    }
}
