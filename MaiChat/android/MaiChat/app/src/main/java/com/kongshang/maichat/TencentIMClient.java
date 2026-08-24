package com.kongshang.maichat;

import android.content.Context;
import android.util.Log;

import com.tencent.imsdk.v2.V2TIMAdvancedMsgListener;
import com.tencent.imsdk.v2.V2TIMCallback;
import com.tencent.imsdk.v2.V2TIMDownloadCallback;
import com.tencent.imsdk.v2.V2TIMElem;
import com.tencent.imsdk.v2.V2TIMFileElem;
import com.tencent.imsdk.v2.V2TIMFriendInfo;
import com.tencent.imsdk.v2.V2TIMFriendOperationResult;
import com.tencent.imsdk.v2.V2TIMImageElem;
import com.tencent.imsdk.v2.V2TIMManager;
import com.tencent.imsdk.v2.V2TIMMessage;
import com.tencent.imsdk.v2.V2TIMSDKConfig;
import com.tencent.imsdk.v2.V2TIMSDKListener;
import com.tencent.imsdk.v2.V2TIMSendCallback;
import com.tencent.imsdk.v2.V2TIMSoundElem;
import com.tencent.imsdk.v2.V2TIMUserFullInfo;
import com.tencent.imsdk.v2.V2TIMUserStatus;
import com.tencent.imsdk.v2.V2TIMValueCallback;
import com.tencent.imsdk.v2.V2TIMVideoElem;

import java.io.File;
import java.io.IOException;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.Map;
import java.util.UUID;

public final class TencentIMClient {
    public enum ConnectionState {
        DISCONNECTED,
        CONNECTING,
        CONNECTED,
        FAILED
    }

    public enum PresenceStatus {
        UNKNOWN,
        ONLINE,
        OFFLINE
    }

    public interface Listener {
        void onConnectionStateChanged(ConnectionState state, String detail);
        void onIncomingMessage(RemoteIMMessage message);
        void onProfilesUpdated(List<RemoteIMContact> contacts);
        void onPresenceUpdated(Map<String, PresenceStatus> statuses);
    }

    public interface SendCompletion {
        void onSuccess(String remoteId, long createdAtMillis);
        void onError(int code, String message);
    }

    public interface OperationCompletion {
        void onSuccess();
        void onError(int code, String message);
    }

    // 日志一律英文：logcat 在不同机器上的编码不一致，中文容易变成乱码，
    // 也不便直接贴进 issue 检索。桌面端 AppLog 用的是同一条约定。
    private static final String TAG = "MaiChat.im";

    private final Context context;
    private final RemoteIMMediaPaths mediaPaths;
    private final Listener listener;
    private final V2TIMAdvancedMsgListener messageListener = new V2TIMAdvancedMsgListener() {
        @Override
        public void onRecvNewMessage(V2TIMMessage message) {
            handleIncomingMessage(message);
        }
    };
    private final V2TIMSDKListener sdkListener = new V2TIMSDKListener() {
        @Override
        public void onConnecting() {
            listener.onConnectionStateChanged(ConnectionState.CONNECTING, "连接中");
        }

        @Override
        public void onConnectSuccess() {
            listener.onConnectionStateChanged(ConnectionState.CONNECTED, "已连接");
        }

        @Override
        public void onConnectFailed(int code, String error) {
            Log.w(TAG, "connect failed: code=" + code + " " + error);
            listener.onConnectionStateChanged(ConnectionState.FAILED, safeError(error, "连接失败"));
        }

        @Override
        public void onKickedOffline() {
            Log.w(TAG, "kicked offline: signed in on another device");
            listener.onConnectionStateChanged(ConnectionState.FAILED, "账号已在其他设备登录");
        }

        @Override
        public void onUserSigExpired() {
            Log.w(TAG, "userSig expired");
            listener.onConnectionStateChanged(ConnectionState.FAILED, "登录凭证已过期，请重新登录");
        }

        @Override
        public void onUserStatusChanged(List<V2TIMUserStatus> statusList) {
            listener.onPresenceUpdated(statusMap(statusList));
        }
    };

    private Integer initializedSdkAppId;
    private String currentUserId = "";
    private String profileRequestKey = "";
    private String presenceRequestKey = "";
    private boolean presenceUnsupported;

    public TencentIMClient(Context context, Listener listener) {
        this.context = context.getApplicationContext();
        this.listener = listener;
        // 收到的媒体必须落在持久目录：放缓存目录的话，系统清缓存或用户手动清除之后，
        // 聊天记录里的图片视频就永远打不开了。
        this.mediaPaths = RemoteIMMediaPaths.forApp(context);
    }

    public void connect(int sdkAppId, String userId, String userSig) {
        String cleanUserId = clean(userId);
        listener.onConnectionStateChanged(ConnectionState.CONNECTING, "连接中");
        if (initializedSdkAppId == null || initializedSdkAppId != sdkAppId) {
            if (initializedSdkAppId != null) {
                V2TIMManager.getMessageManager().removeAdvancedMsgListener(messageListener);
                V2TIMManager.getInstance().removeIMSDKListener(sdkListener);
                V2TIMManager.getInstance().unInitSDK();
            }
            boolean initialized = V2TIMManager.getInstance().initSDK(
                context,
                sdkAppId,
                new V2TIMSDKConfig()
            );
            if (!initialized) {
                Log.e(TAG, "SDK init failed: sdkAppId=" + sdkAppId);
                listener.onConnectionStateChanged(ConnectionState.FAILED, "IM SDK 初始化失败");
                return;
            }
            initializedSdkAppId = sdkAppId;
            V2TIMManager.getInstance().addIMSDKListener(sdkListener);
            V2TIMManager.getMessageManager().addAdvancedMsgListener(messageListener);
        }
        currentUserId = cleanUserId;
        Log.i(TAG, "connect: sdkAppId=" + sdkAppId + " user=" + cleanUserId);
        V2TIMManager.getInstance().login(cleanUserId, userSig, new V2TIMCallback() {
            @Override
            public void onSuccess() {
                Log.i(TAG, "login ok: " + cleanUserId);
                listener.onConnectionStateChanged(ConnectionState.CONNECTED, "已连接");
            }

            @Override
            public void onError(int code, String description) {
                Log.w(TAG, "login failed: code=" + code + " " + description);
                listener.onConnectionStateChanged(
                    ConnectionState.FAILED,
                    safeError(description, "登录失败（" + code + "）")
                );
            }
        });
    }

    public void disconnect(OperationCompletion completion) {
        V2TIMManager.getInstance().logout(callback(completion));
        currentUserId = "";
    }

    public void destroy() {
        V2TIMManager.getMessageManager().removeAdvancedMsgListener(messageListener);
        V2TIMManager.getInstance().removeIMSDKListener(sdkListener);
    }

    public void sendText(
        String peerId,
        String text,
        RemoteIMOrigin origin,
        SendCompletion completion
    ) {
        V2TIMMessage message = V2TIMManager.getMessageManager().createTextMessage(text);
        send(message, peerId, origin, completion);
    }

    public void sendImage(
        String peerId,
        String path,
        RemoteIMOrigin origin,
        SendCompletion completion
    ) {
        V2TIMMessage message = V2TIMManager.getMessageManager().createImageMessage(path);
        send(message, peerId, origin, completion);
    }

    public void sendVoice(
        String peerId,
        String path,
        int durationSeconds,
        RemoteIMOrigin origin,
        SendCompletion completion
    ) {
        V2TIMMessage message = V2TIMManager.getMessageManager().createSoundMessage(
            path,
            Math.max(1, durationSeconds)
        );
        send(message, peerId, origin, completion);
    }

    public void sendFile(
        String peerId,
        String path,
        String fileName,
        RemoteIMOrigin origin,
        SendCompletion completion
    ) {
        V2TIMMessage message = V2TIMManager.getMessageManager().createFileMessage(path, fileName);
        send(message, peerId, origin, completion);
    }

    public void clearHistory(String peerId, OperationCompletion completion) {
        V2TIMManager.getMessageManager().clearC2CHistoryMessage(peerId, callback(completion));
    }

    public void deleteContact(String peerId, OperationCompletion completion) {
        V2TIMManager.getFriendshipManager().deleteFromFriendList(
            Collections.singletonList(peerId),
            V2TIMFriendInfo.V2TIM_FRIEND_TYPE_BOTH,
            new V2TIMValueCallback<List<V2TIMFriendOperationResult>>() {
                @Override
                public void onSuccess(List<V2TIMFriendOperationResult> results) {
                    if (results != null) {
                        for (V2TIMFriendOperationResult result : results) {
                            if (result.getResultCode() != 0) {
                                completion.onError(result.getResultCode(), result.getResultInfo());
                                return;
                            }
                        }
                    }
                    completion.onSuccess();
                }

                @Override
                public void onError(int code, String description) {
                    completion.onError(code, description);
                }
            }
        );
    }

    public void refreshProfiles(List<String> userIds) {
        List<String> cleaned = cleanUserIds(userIds);
        if (cleaned.isEmpty()) return;
        String accountAtRequest = currentUserId;
        String requestKey = accountAtRequest + "\n" + String.join("\n", cleaned);
        synchronized (this) {
            if (requestKey.equals(profileRequestKey)) return;
            profileRequestKey = requestKey;
        }
        V2TIMManager.getInstance().getUsersInfo(
            cleaned,
            new V2TIMValueCallback<List<V2TIMUserFullInfo>>() {
                @Override
                public void onSuccess(List<V2TIMUserFullInfo> users) {
                    synchronized (TencentIMClient.this) {
                        if (requestKey.equals(profileRequestKey)) profileRequestKey = "";
                    }
                    if (!accountAtRequest.equals(currentUserId)) return;
                    List<RemoteIMContact> contacts = new ArrayList<>();
                    if (users != null) {
                        for (V2TIMUserFullInfo user : users) {
                            String userId = clean(user.getUserID());
                            if (userId.isEmpty()) continue;
                            String nickname = clean(user.getNickName());
                            contacts.add(new RemoteIMContact(
                                userId,
                                nickname.isEmpty() ? userId : nickname,
                                clean(user.getFaceUrl())
                            ));
                        }
                    }
                    listener.onProfilesUpdated(contacts);
                }

                @Override
                public void onError(int code, String description) {
                    synchronized (TencentIMClient.this) {
                        if (requestKey.equals(profileRequestKey)) profileRequestKey = "";
                    }
                    // Profile refresh is best effort and must not interrupt messaging.
                }
            }
        );
    }

    public void refreshAndSubscribePresence(List<String> userIds) {
        List<String> cleaned = cleanUserIds(userIds);
        if (cleaned.isEmpty() || presenceUnsupported) return;
        String accountAtRequest = currentUserId;
        String requestKey = accountAtRequest + "\n" + String.join("\n", cleaned);
        synchronized (this) {
            if (requestKey.equals(presenceRequestKey) || presenceUnsupported) return;
            presenceRequestKey = requestKey;
        }
        V2TIMManager.getInstance().getUserStatus(
            cleaned,
            new V2TIMValueCallback<List<V2TIMUserStatus>>() {
                @Override
                public void onSuccess(List<V2TIMUserStatus> statuses) {
                    synchronized (TencentIMClient.this) {
                        if (requestKey.equals(presenceRequestKey)) presenceRequestKey = "";
                    }
                    if (!accountAtRequest.equals(currentUserId)) return;
                    listener.onPresenceUpdated(statusMap(statuses));
                }

                @Override
                public void onError(int code, String description) {
                    synchronized (TencentIMClient.this) {
                        if (isPresenceUnsupported(description)) presenceUnsupported = true;
                        if (requestKey.equals(presenceRequestKey)) presenceRequestKey = "";
                    }
                }
            }
        );
        V2TIMManager.getInstance().subscribeUserStatus(cleaned, new V2TIMCallback() {
            @Override
            public void onSuccess() {
            }

            @Override
            public void onError(int code, String description) {
                synchronized (TencentIMClient.this) {
                    if (isPresenceUnsupported(description)) presenceUnsupported = true;
                    if (requestKey.equals(presenceRequestKey)) presenceRequestKey = "";
                }
            }
        });
    }

    private void send(
        V2TIMMessage message,
        String peerId,
        RemoteIMOrigin origin,
        SendCompletion completion
    ) {
        if (message == null) {
            Log.w(TAG, "send aborted: SDK returned a null message");
            completion.onError(-1, "消息创建失败");
            return;
        }
        message.setCloudCustomData(RemoteIMProtocolMetadata.encode(origin));
        V2TIMManager.getMessageManager().sendMessage(
            message,
            clean(peerId),
            null,
            V2TIMMessage.V2TIM_PRIORITY_DEFAULT,
            false,
            null,
            new V2TIMSendCallback<V2TIMMessage>() {
                @Override
                public void onProgress(int progress) {
                }

                @Override
                public void onSuccess(V2TIMMessage sentMessage) {
                    V2TIMMessage result = sentMessage == null ? message : sentMessage;
                    completion.onSuccess(
                        clean(result.getMsgID()),
                        result.getTimestamp() > 0
                            ? result.getTimestamp() * 1000L
                            : System.currentTimeMillis()
                    );
                }

                @Override
                public void onError(int code, String description) {
                    Log.w(TAG, "send failed: peer=" + clean(peerId)
                        + " code=" + code + " " + description);
                    completion.onError(code, safeError(description, "发送失败"));
                }
            }
        );
    }

    private void handleIncomingMessage(V2TIMMessage sdkMessage) {
        if (sdkMessage == null) {
            Log.w(TAG, "recv: null message from SDK; dropped");
            return;
        }
        if (sdkMessage.isSelf()) return;
        String fromUserId = clean(sdkMessage.getSender());
        if (fromUserId.isEmpty()) fromUserId = clean(sdkMessage.getUserID());
        if (fromUserId.isEmpty()) {
            Log.w(TAG, "recv: message has no sender id; dropped id="
                + clean(sdkMessage.getMsgID()));
            return;
        }
        long createdAt = sdkMessage.getTimestamp() > 0
            ? sdkMessage.getTimestamp() * 1000L
            : System.currentTimeMillis();
        String recipientUserId = currentUserId;
        String messageId = clean(sdkMessage.getMsgID());
        RemoteIMOrigin origin = RemoteIMProtocolMetadata.decode(sdkMessage.getCloudCustomData());

        if (sdkMessage.getTextElem() != null) {
            listener.onIncomingMessage(new RemoteIMMessage(
                null,
                messageId,
                fromUserId,
                recipientUserId,
                sdkMessage.getTextElem().getText(),
                RemoteIMMessage.Direction.INCOMING,
                RemoteIMMessage.Status.RECEIVED,
                createdAt,
                null,
                null,
                null,
                null,
                origin
            ));
            return;
        }
        if (sdkMessage.getImageElem() != null) {
            downloadImage(sdkMessage, fromUserId, recipientUserId, createdAt, origin);
            return;
        }
        if (sdkMessage.getSoundElem() != null) {
            downloadVoice(sdkMessage, fromUserId, recipientUserId, createdAt, origin);
            return;
        }
        if (sdkMessage.getVideoElem() != null) {
            downloadVideo(sdkMessage, fromUserId, recipientUserId, createdAt, origin);
            return;
        }
        if (sdkMessage.getFileElem() != null) {
            downloadFile(sdkMessage, fromUserId, recipientUserId, createdAt, origin);
            return;
        }
        // 一个分支都没命中就说明这类消息 Android 端根本不认，而且原来是悄悄丢掉的。
        // elemType 打出来才能一眼看出缺哪类；例如视频（V2TIM_ELEM_TYPE_VIDEO）目前
        // 就没有对应分支。
        Log.w(TAG, "recv: unhandled message, dropped."
            + " from=" + fromUserId
            + " id=" + messageId
            + " elemType=" + sdkMessage.getElemType()
            + " <- no branch matched; this message type is not implemented");
    }

    private void downloadImage(
        V2TIMMessage message,
        String fromUserId,
        String recipientUserId,
        long createdAt,
        RemoteIMOrigin origin
    ) {
        V2TIMImageElem elem = message.getImageElem();
        V2TIMImageElem.V2TIMImage selected = null;
        for (V2TIMImageElem.V2TIMImage image : elem.getImageList()) {
            if (selected == null || imageScore(image) > imageScore(selected)) selected = image;
        }
        if (selected == null) {
            Log.w(TAG, "recv image: elem carries no image entry; dropped id="
                + clean(message.getMsgID()));
            return;
        }
        String remoteId = clean(selected.getUUID());
        if (remoteId.isEmpty()) remoteId = clean(message.getMsgID());
        File target = mediaFile(RemoteIMMediaPaths.IMAGES, remoteId, extension(selected.getUrl(), "jpg"));
        V2TIMImageElem.V2TIMImage image = selected;
        String finalRemoteId = remoteId;
        selected.downloadImage(target.getAbsolutePath(), new V2TIMDownloadCallback() {
            @Override
            public void onProgress(V2TIMElem.V2ProgressInfo progressInfo) {
            }

            @Override
            public void onSuccess() {
                RemoteIMImageAttachment attachment = new RemoteIMImageAttachment(
                    target.getAbsolutePath(),
                    image.getWidth(),
                    image.getHeight(),
                    image.getSize()
                );
                listener.onIncomingMessage(new RemoteIMMessage(
                    null,
                    finalRemoteId,
                    fromUserId,
                    recipientUserId,
                    "[图片消息] " + target.getName(),
                    RemoteIMMessage.Direction.INCOMING,
                    RemoteIMMessage.Status.RECEIVED,
                    createdAt,
                    attachment,
                    null,
                    null,
                    null,
                    origin
                ));
            }

            @Override
            public void onError(int code, String description) {
                Log.w(TAG, "download image failed: code=" + code + " " + description
                    + " target=" + target.getAbsolutePath());
            }
        });
    }

    private void downloadVoice(
        V2TIMMessage message,
        String fromUserId,
        String recipientUserId,
        long createdAt,
        RemoteIMOrigin origin
    ) {
        V2TIMSoundElem elem = message.getSoundElem();
        String remoteId = clean(elem.getUUID());
        if (remoteId.isEmpty()) remoteId = clean(message.getMsgID());
        File target = mediaFile(RemoteIMMediaPaths.VOICES, remoteId, "m4a");
        int duration = Math.max(1, elem.getDuration());
        String finalRemoteId = remoteId;
        elem.downloadSound(target.getAbsolutePath(), new V2TIMDownloadCallback() {
            @Override
            public void onProgress(V2TIMElem.V2ProgressInfo progressInfo) {
            }

            @Override
            public void onSuccess() {
                listener.onIncomingMessage(new RemoteIMMessage(
                    null,
                    finalRemoteId,
                    fromUserId,
                    recipientUserId,
                    "[语音消息 " + duration + "s]",
                    RemoteIMMessage.Direction.INCOMING,
                    RemoteIMMessage.Status.RECEIVED,
                    createdAt,
                    null,
                    new RemoteIMVoiceAttachment(target.getAbsolutePath(), duration),
                    null,
                    null,
                    origin
                ));
            }

            @Override
            public void onError(int code, String description) {
                Log.w(TAG, "download voice failed: code=" + code + " " + description
                    + " target=" + target.getAbsolutePath());
            }
        });
    }

    private void downloadVideo(
        V2TIMMessage message,
        String fromUserId,
        String recipientUserId,
        long createdAt,
        RemoteIMOrigin origin
    ) {
        V2TIMVideoElem elem = message.getVideoElem();
        String remoteId = clean(elem.getVideoUUID());
        if (remoteId.isEmpty()) remoteId = clean(message.getMsgID());
        final String finalRemoteId = remoteId;

        final File videoTarget = mediaFile(RemoteIMMediaPaths.VIDEOS, remoteId, extension(elem.getVideoPath(), "mp4"));
        // 先下到 .part 再改名：这样"目标文件存在"就等价于"已经下完"，界面只需判存在性，
        // 不会拿到一个下了一半的文件去播放。
        final File videoPart = new File(videoTarget.getAbsolutePath() + ".part");
        final File coverTarget = mediaFile(RemoteIMMediaPaths.VIDEO_COVERS, remoteId + "-cover", "jpg");

        final int duration = Math.max(0, (int) elem.getDuration());
        final int width = Math.max(0, elem.getSnapshotWidth());
        final int height = Math.max(0, elem.getSnapshotHeight());
        final long sizeBytes = Math.max(0L, elem.getVideoSize());

        // 视频体积大、下得慢，等它下完再出气泡会让聊天界面长时间什么都不显示。
        // 封面一到就先把消息投出去；视频路径指向最终目标，下完即可播放。
        // 封面失败时退回等视频下完再投，保证消息不会因为没有封面而丢失。
        final AtomicBoolean delivered = new AtomicBoolean(false);

        elem.downloadSnapshot(coverTarget.getAbsolutePath(), new V2TIMDownloadCallback() {
            @Override
            public void onProgress(V2TIMElem.V2ProgressInfo progressInfo) {
            }

            @Override
            public void onSuccess() {
                if (delivered.compareAndSet(false, true)) {
                    emitVideoMessage(
                        finalRemoteId, fromUserId, recipientUserId, createdAt, origin,
                        videoTarget.getAbsolutePath(), coverTarget.getAbsolutePath(),
                        duration, width, height, sizeBytes
                    );
                }
            }

            @Override
            public void onError(int code, String description) {
                Log.w(TAG, "download video cover failed: code=" + code + " " + description
                    + " target=" + coverTarget.getAbsolutePath());
            }
        });

        elem.downloadVideo(videoPart.getAbsolutePath(), new V2TIMDownloadCallback() {
            @Override
            public void onProgress(V2TIMElem.V2ProgressInfo progressInfo) {
            }

            @Override
            public void onSuccess() {
                if (videoTarget.exists() && !videoTarget.delete()) {
                    Log.w(TAG, "recv video: cannot replace existing file "
                        + videoTarget.getAbsolutePath());
                }
                if (!videoPart.renameTo(videoTarget)) {
                    Log.w(TAG, "recv video: rename failed, "
                        + videoPart.getAbsolutePath() + " -> " + videoTarget.getAbsolutePath());
                    return;
                }
                // 封面失败或还没回来时，这里兜底把消息投出去。
                if (delivered.compareAndSet(false, true)) {
                    emitVideoMessage(
                        finalRemoteId, fromUserId, recipientUserId, createdAt, origin,
                        videoTarget.getAbsolutePath(),
                        coverTarget.exists() ? coverTarget.getAbsolutePath() : "",
                        duration, width, height, sizeBytes
                    );
                }
            }

            @Override
            public void onError(int code, String description) {
                Log.w(TAG, "download video failed: code=" + code + " " + description
                    + " target=" + videoTarget.getAbsolutePath());
                if (!videoPart.exists()) return;
                if (!videoPart.delete()) {
                    Log.w(TAG, "recv video: cannot delete partial file "
                        + videoPart.getAbsolutePath());
                }
            }
        });
    }

    private void emitVideoMessage(
        String remoteId,
        String fromUserId,
        String recipientUserId,
        long createdAt,
        RemoteIMOrigin origin,
        String videoPath,
        String coverPath,
        int duration,
        int width,
        int height,
        long sizeBytes
    ) {
        RemoteIMVideoAttachment attachment = new RemoteIMVideoAttachment(
            videoPath, coverPath, duration, width, height, sizeBytes
        );
        listener.onIncomingMessage(new RemoteIMMessage(
            null,
            remoteId,
            fromUserId,
            recipientUserId,
            "[视频消息 " + duration + "s]",
            RemoteIMMessage.Direction.INCOMING,
            RemoteIMMessage.Status.RECEIVED,
            createdAt,
            null,
            null,
            null,
            attachment,
            origin
        ));
    }

    private void downloadFile(
        V2TIMMessage message,
        String fromUserId,
        String recipientUserId,
        long createdAt,
        RemoteIMOrigin origin
    ) {
        V2TIMFileElem elem = message.getFileElem();
        String remoteId = clean(elem.getUUID());
        if (remoteId.isEmpty()) remoteId = clean(message.getMsgID());
        String fileName = safeFileName(elem.getFileName(), "remote-im-file.bin");
        File target = mediaFile(RemoteIMMediaPaths.FILES, remoteId, extension(fileName, "bin"));
        String finalRemoteId = remoteId;
        elem.downloadFile(target.getAbsolutePath(), new V2TIMDownloadCallback() {
            @Override
            public void onProgress(V2TIMElem.V2ProgressInfo progressInfo) {
            }

            @Override
            public void onSuccess() {
                RemoteIMFileAttachment attachment = new RemoteIMFileAttachment(
                    target.getAbsolutePath(),
                    fileName,
                    mimeType(fileName),
                    elem.getFileSize()
                );
                listener.onIncomingMessage(new RemoteIMMessage(
                    null,
                    finalRemoteId,
                    fromUserId,
                    recipientUserId,
                    "[文件消息] " + fileName,
                    RemoteIMMessage.Direction.INCOMING,
                    RemoteIMMessage.Status.RECEIVED,
                    createdAt,
                    null,
                    null,
                    attachment,
                    null,
                    origin
                ));
            }

            @Override
            public void onError(int code, String description) {
                Log.w(TAG, "download file failed: code=" + code + " " + description
                    + " target=" + target.getAbsolutePath());
            }
        });
    }

    private File mediaFile(String kind, String remoteId, String extension) {
        String name = safeFileName(remoteId, UUID.randomUUID().toString());
        File directory;
        try {
            directory = mediaPaths.directory(RemoteIMMediaPaths.INCOMING, kind);
        } catch (IOException error) {
            // 目录建不出来时仍返回目标路径：写入随后会失败并由下载回调统一报错，
            // 比静默改写到别处更容易定位。
            Log.w(TAG, "media: create directory failed."
                + " kind=" + kind
                + " <- attachment save will fail", error);
            directory = new File(new File(mediaPaths.root(), RemoteIMMediaPaths.INCOMING), kind);
        }
        return new File(directory, name + "." + extension);
    }

    private static int imageScore(V2TIMImageElem.V2TIMImage image) {
        if (image.getSize() > 0) return image.getSize();
        return Math.max(0, image.getWidth()) * Math.max(0, image.getHeight());
    }

    private static Map<String, PresenceStatus> statusMap(List<V2TIMUserStatus> statuses) {
        Map<String, PresenceStatus> result = new HashMap<>();
        if (statuses == null) return result;
        for (V2TIMUserStatus status : statuses) {
            String userId = clean(status.getUserID());
            if (userId.isEmpty()) continue;
            PresenceStatus value = PresenceStatus.UNKNOWN;
            if (status.getStatusType() == V2TIMUserStatus.V2TIM_USER_STATUS_ONLINE) {
                value = PresenceStatus.ONLINE;
            } else if (status.getStatusType() == V2TIMUserStatus.V2TIM_USER_STATUS_OFFLINE
                || status.getStatusType() == V2TIMUserStatus.V2TIM_USER_STATUS_UNLOGINED) {
                value = PresenceStatus.OFFLINE;
            }
            result.put(userId, value);
        }
        return result;
    }

    private static List<String> cleanUserIds(List<String> userIds) {
        if (userIds == null) return Collections.emptyList();
        List<String> result = new ArrayList<>();
        for (String userId : userIds) {
            String cleanUserId = clean(userId);
            if (!cleanUserId.isEmpty() && !result.contains(cleanUserId)) result.add(cleanUserId);
        }
        return result;
    }

    private static V2TIMCallback callback(OperationCompletion completion) {
        return new V2TIMCallback() {
            @Override
            public void onSuccess() {
                completion.onSuccess();
            }

            @Override
            public void onError(int code, String description) {
                completion.onError(code, safeError(description, "操作失败"));
            }
        };
    }

    private static String extension(String path, String fallback) {
        String cleanPath = clean(path);
        int query = cleanPath.indexOf('?');
        if (query >= 0) cleanPath = cleanPath.substring(0, query);
        int slash = Math.max(cleanPath.lastIndexOf('/'), cleanPath.lastIndexOf('\\'));
        int dot = cleanPath.lastIndexOf('.');
        if (dot <= slash || dot == cleanPath.length() - 1) return fallback;
        String value = cleanPath.substring(dot + 1).toLowerCase(Locale.ROOT);
        return value.matches("[a-z0-9]{1,8}") ? value : fallback;
    }

    private static String mimeType(String fileName) {
        String extension = extension(fileName, "");
        if ("md".equals(extension) || "markdown".equals(extension)) return "text/markdown";
        if ("html".equals(extension) || "htm".equals(extension)) return "text/html";
        if ("txt".equals(extension)) return "text/plain";
        if ("json".equals(extension)) return "application/json";
        if ("pdf".equals(extension)) return "application/pdf";
        return "application/octet-stream";
    }

    private static String safeFileName(String value, String fallback) {
        String cleanValue = clean(value).replaceAll("[^A-Za-z0-9._-]", "_");
        return cleanValue.isEmpty() ? fallback : cleanValue;
    }

    private static String safeError(String value, String fallback) {
        String cleanValue = clean(value);
        return cleanValue.isEmpty() ? fallback : cleanValue;
    }

    private static boolean isPresenceUnsupported(String description) {
        String value = clean(description).toLowerCase(Locale.ROOT);
        return value.contains("premium")
            || value.contains("upgrade")
            || value.contains("旗舰")
            || value.contains("套餐");
    }

    private static String clean(String value) {
        return value == null ? "" : value.trim();
    }
}
