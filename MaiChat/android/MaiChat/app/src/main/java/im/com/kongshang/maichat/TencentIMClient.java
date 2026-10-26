// 文件作用：Android IM 身份、消息同步与本地历史；TencentIMClient 负责外部接口请求与结果转换，本文件给出其实现。
// 主要入口：onRecvNewMessage、onConnecting、onConnectSuccess、onConnectFailed。
package com.kongshang.maichat;

import android.content.Context;
import android.util.Log;

import com.tencent.imsdk.v2.V2TIMAdvancedMsgListener;
import com.tencent.imsdk.v2.V2TIMCallback;
import com.tencent.imsdk.v2.V2TIMElem;
import com.tencent.imsdk.v2.V2TIMFileElem;
import com.tencent.imsdk.v2.V2TIMFriendAddApplication;
import com.tencent.imsdk.v2.V2TIMFriendApplication;
import com.tencent.imsdk.v2.V2TIMFriendApplicationResult;
import com.tencent.imsdk.v2.V2TIMFriendInfo;
import com.tencent.imsdk.v2.V2TIMFriendOperationResult;
import com.tencent.imsdk.v2.V2TIMImageElem;
import com.tencent.imsdk.v2.V2TIMManager;
import com.tencent.imsdk.v2.V2TIMMessage;
import com.tencent.imsdk.v2.V2TIMSDKConfig;
import com.tencent.imsdk.v2.V2TIMSDKListener;
import com.tencent.imsdk.v2.V2TIMSendCallback;
import com.tencent.imsdk.v2.V2TIMSoundElem;
import com.tencent.imsdk.v2.V2TIMTextElem;
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
        default void onIncomingActivity(String sender, String recipient, RemoteIMActivitySignal signal) { }
        default void onVideoMediaUpdated(
                String sender, String recipient, String remoteId, RemoteIMVideoAttachment attachment) {}
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

    public interface FriendRequestsCompletion {
        void onSuccess(List<RemoteIMFriendRequest> requests);
        void onError(int code, String message);
    }

    public interface FriendListCompletion {
        void onSuccess(List<String> userIds);
        void onError(int code, String message);
    }

    // 日志一律英文：logcat 在不同机器上的编码不一致，中文容易变成乱码，
    // 也不便直接贴进 issue 检索。桌面端 AppLog 用的是同一条约定。
    private static final String TAG = "MaiChat.im";
    private final android.os.HandlerThread sdkThread = new android.os.HandlerThread("MaiChat-IM");
    private final android.os.Handler sdkHandler;
    private volatile boolean closed;
    private final Context context;
    private final RemoteIMMediaPaths mediaPaths;
    private final MaiChatAttachmentDownload.Transport mediaDownloader;
    private volatile MaiChatAttachmentDownload.Scope mediaScope = new MaiChatAttachmentDownload.Scope();
    private final Listener listener;
    private final V2TIMAdvancedMsgListener messageListener = new V2TIMAdvancedMsgListener() {
        @Override
        public void onRecvNewMessage(V2TIMMessage message) {
            String owner = currentUserId;
            sdkHandler.post(() -> { if (!closed && owner.equals(currentUserId)) handleIncomingMessage(message); });
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
    private volatile String currentUserId = "";
    private String profileRequestKey = "";
    private String presenceRequestKey = "";
    private boolean presenceUnsupported;

    public TencentIMClient(Context context, Listener listener) {
        this(context, listener, new MaiChatAttachmentDownload.NativeTransport());
    }

    TencentIMClient(Context context, Listener listener,
                    MaiChatAttachmentDownload.Transport mediaDownloader) {
        sdkThread.start(); sdkHandler = new android.os.Handler(sdkThread.getLooper());
        this.context = context.getApplicationContext();
        this.listener = listener;
        this.mediaDownloader = mediaDownloader;
        // 收到的媒体必须落在持久目录：放缓存目录的话，系统清缓存或用户手动清除之后，
        // 聊天记录里的图片视频就永远打不开了。
        this.mediaPaths = RemoteIMMediaPaths.forApp(context);
    }

    public void connect(int sdkAppId, String userId, String userSig) {
        if (closed) return;
        if (android.os.Looper.myLooper() != sdkThread.getLooper()) {
            sdkHandler.post(() -> connect(sdkAppId, userId, userSig)); return;
        }
        String cleanUserId = clean(userId);
        mediaScope.cancel();
        mediaScope = new MaiChatAttachmentDownload.Scope();
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
                V2TIMUserFullInfo profile = new V2TIMUserFullInfo();
                profile.setAllowType(V2TIMUserFullInfo.V2TIM_FRIEND_NEED_CONFIRM);
                V2TIMManager.getInstance().setSelfInfo(profile, new V2TIMCallback() {
                    @Override public void onSuccess() { }
                    @Override public void onError(int code, String message) {
                        Log.w(TAG, "friend verification update failed: " + code);
                    }
                });
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
        if (closed) return;
        mediaScope.cancel();
        if (android.os.Looper.myLooper() != sdkThread.getLooper()) {
            sdkHandler.post(() -> disconnect(completion)); return;
        }
        V2TIMManager.getInstance().logout(callback(completion));
        currentUserId = "";
    }

    public void destroy() {
        closed = true; currentUserId = "";
        mediaScope.cancel();
        sdkHandler.post(() -> {
            if (initializedSdkAppId != null) {
                V2TIMManager.getMessageManager().removeAdvancedMsgListener(messageListener);
                V2TIMManager.getInstance().removeIMSDKListener(sdkListener);
            }
            sdkThread.quitSafely();
        });
    }

    public void sendText(
        String peerId,
        String text,
        RemoteIMOrigin origin,
        SendCompletion completion
    ) {
        if (closed) return;
        if (android.os.Looper.myLooper() != sdkThread.getLooper()) {
            sdkHandler.post(() -> sendText(peerId, text, origin, completion)); return;
        }
        sendText(peerId, text, origin, null, completion);
    }

    public void sendText(
        String peerId,
        String text,
        RemoteIMOrigin origin,
        RemoteIMQuote quote,
        SendCompletion completion
    ) {
        if (closed) return;
        if (android.os.Looper.myLooper() != sdkThread.getLooper()) {
            sdkHandler.post(() -> sendText(peerId, text, origin, quote, completion)); return;
        }
        V2TIMMessage message = V2TIMManager.getMessageManager().createTextMessage(text);
        send(message, peerId, RemoteIMProtocolMetadata.encode(origin, quote), completion);
    }

    public void sendApprovalDecision(
        String peerId,
        RemoteIMApprovalDecision decision,
        SendCompletion completion
    ) {
        if (closed) return;
        if (android.os.Looper.myLooper() != sdkThread.getLooper()) {
            sdkHandler.post(() -> sendApprovalDecision(peerId, decision, completion)); return;
        }
        if (decision == null) {
            completion.onError(-1, "审批操作无效");
            return;
        }
        V2TIMMessage message = V2TIMManager.getMessageManager().createTextMessage(
            decision.action().decisionDisplayText()
        );
        Log.i(TAG, "approval: decision send start. peer=" + clean(peerId)
            + " action=" + decision.action().wireValue());
        send(
            message,
            peerId,
            RemoteIMProtocolMetadata.encodeApprovalDecision(decision),
            new SendCompletion() {
                @Override
                public void onSuccess(String remoteId, long createdAtMillis) {
                    Log.i(TAG, "approval: decision send resolved. peer=" + clean(peerId)
                        + " action=" + decision.action().wireValue());
                    completion.onSuccess(remoteId, createdAtMillis);
                }

                @Override
                public void onError(int code, String description) {
                    Log.w(TAG, "approval: decision send failed. peer=" + clean(peerId)
                        + " action=" + decision.action().wireValue()
                        + " code=" + code);
                    completion.onError(code, description);
                }
            }
        );
    }

    public void sendImage(String peerId, String path, RemoteIMOrigin origin, SendCompletion completion) {
        sendImage(peerId, path, origin, null, completion);
    }
    public void sendImage(
            String peerId, String path, RemoteIMOrigin origin, RemoteIMQuote quote, SendCompletion completion) {
        if (closed) return;
        if (android.os.Looper.myLooper() != sdkThread.getLooper()) {
            sdkHandler.post(() -> sendImage(peerId, path, origin, quote, completion)); return;
        }
        V2TIMMessage message = V2TIMManager.getMessageManager().createImageMessage(path);
        send(message, peerId, RemoteIMProtocolMetadata.encode(origin, quote), completion);
    }

    public void sendVoice(
            String peerId, String path, int durationSeconds, RemoteIMOrigin origin, SendCompletion completion) {
        sendVoice(peerId, path, durationSeconds, origin, null, completion);
    }
    public void sendVoice(String peerId, String path, int durationSeconds, RemoteIMOrigin origin, RemoteIMQuote quote,
            SendCompletion completion) {
        if (closed) return;
        if (android.os.Looper.myLooper() != sdkThread.getLooper()) {
            sdkHandler.post(() -> sendVoice(peerId, path, durationSeconds, origin, quote, completion)); return;
        }
        V2TIMMessage message = V2TIMManager.getMessageManager().createSoundMessage(path, Math.max(1, durationSeconds));
        send(message, peerId, RemoteIMProtocolMetadata.encode(origin, quote), completion);
    }

    public void sendFile(
            String peerId, String path, String fileName, RemoteIMOrigin origin, SendCompletion completion) {
        sendFile(peerId, path, fileName, origin, null, completion);
    }
    public void sendFile(String peerId, String path, String fileName, RemoteIMOrigin origin, RemoteIMQuote quote,
            SendCompletion completion) {
        if (closed) return;
        if (android.os.Looper.myLooper() != sdkThread.getLooper()) {
            sdkHandler.post(() -> sendFile(peerId, path, fileName, origin, quote, completion)); return;
        }
        V2TIMMessage message = V2TIMManager.getMessageManager().createFileMessage(path, fileName);
        send(message, peerId, RemoteIMProtocolMetadata.encode(origin, quote), completion);
    }

    public void sendVideo(
            String peerId, RemoteIMVideoAttachment attachment, RemoteIMOrigin origin, SendCompletion completion) {
        sendVideo(peerId, attachment, origin, null, completion);
    }
    public void sendVideo(String peerId, RemoteIMVideoAttachment attachment, RemoteIMOrigin origin, RemoteIMQuote quote,
            SendCompletion completion) {
        if (closed) return;
        if (android.os.Looper.myLooper() != sdkThread.getLooper()) {
            sdkHandler.post(() -> sendVideo(peerId, attachment, origin, quote, completion)); return;
        }
        String path = attachment.localPath();
        int dot = path.lastIndexOf('.');
        String type = dot >= 0 ? path.substring(dot + 1).toLowerCase(Locale.ROOT) : "mp4";
        V2TIMMessage message = V2TIMManager.getMessageManager().createVideoMessage(
                path, type, attachment.durationSeconds(), attachment.coverPath());
        send(message, peerId, RemoteIMProtocolMetadata.encode(origin, quote), completion);
    }

    public void clearHistory(String peerId, OperationCompletion completion) {
        if (closed) return;
        if (android.os.Looper.myLooper() != sdkThread.getLooper()) {
            sdkHandler.post(() -> clearHistory(peerId, completion)); return;
        }
        V2TIMManager.getMessageManager().clearC2CHistoryMessage(peerId, callback(completion));
    }

    public void requestFriend(String peerId, OperationCompletion completion) {
        if (closed) return;
        if (android.os.Looper.myLooper() != sdkThread.getLooper()) {
            sdkHandler.post(() -> requestFriend(peerId, completion)); return;
        }
        V2TIMFriendAddApplication application = new V2TIMFriendAddApplication(peerId);
        application.setAddType(V2TIMFriendInfo.V2TIM_FRIEND_TYPE_BOTH);
        application.setAddSource("AddSource_Type_MaiChat");
        application.setAddWording("请求添加为好友");
        V2TIMManager.getFriendshipManager().addFriend(application,
            new V2TIMValueCallback<V2TIMFriendOperationResult>() {
                @Override public void onSuccess(V2TIMFriendOperationResult result) {
                    if (result == null || result.getResultCode() == 0 ||
                        result.getResultCode() == 30539) completion.onSuccess();
                    else completion.onError(result.getResultCode(), result.getResultInfo());
                }
                @Override public void onError(int code, String message) {
                    if (code == 30539) completion.onSuccess();
                    else completion.onError(code, message);
                }
            });
    }

    public void friendRequests(FriendRequestsCompletion completion) {
        if (closed) return;
        if (android.os.Looper.myLooper() != sdkThread.getLooper()) {
            sdkHandler.post(() -> friendRequests(completion)); return;
        }
        V2TIMManager.getFriendshipManager().getFriendApplicationList(
            new V2TIMValueCallback<V2TIMFriendApplicationResult>() {
                @Override public void onSuccess(V2TIMFriendApplicationResult result) {
                    List<RemoteIMFriendRequest> requests = new ArrayList<>();
                    if (result != null && result.getFriendApplicationList() != null) {
                        for (V2TIMFriendApplication application : result.getFriendApplicationList()) {
                            if (application.getType() != V2TIMFriendApplication.V2TIM_FRIEND_APPLICATION_COME_IN)
                                continue;
                            String userId = application.getUserID();
                            if (userId == null || userId.isEmpty()) continue;
                            String name = application.getNickname();
                            requests.add(new RemoteIMFriendRequest(userId,
                                name == null || name.isEmpty() ? userId : name,
                                application.getFaceUrl(), application.getAddWording()));
                        }
                    }
                    completion.onSuccess(requests);
                }
                @Override public void onError(int code, String message) {
                    completion.onError(code, message);
                }
            });
    }

    public void friendUserIds(FriendListCompletion completion) {
        if (closed) return;
        if (android.os.Looper.myLooper() != sdkThread.getLooper()) {
            sdkHandler.post(() -> friendUserIds(completion)); return;
        }
        V2TIMManager.getFriendshipManager().getFriendList(
            new V2TIMValueCallback<List<V2TIMFriendInfo>>() {
                @Override public void onSuccess(List<V2TIMFriendInfo> friends) {
                    List<String> userIds = new ArrayList<>();
                    if (friends != null) for (V2TIMFriendInfo friend : friends) {
                        if (friend.getUserID() != null) userIds.add(friend.getUserID());
                    }
                    completion.onSuccess(userIds);
                }
                @Override public void onError(int code, String message) {
                    completion.onError(code, message);
                }
            });
    }

    public void decideFriendRequest(String userId, boolean accept, OperationCompletion completion) {
        if (closed) return;
        if (android.os.Looper.myLooper() != sdkThread.getLooper()) {
            sdkHandler.post(() -> decideFriendRequest(userId, accept, completion)); return;
        }
        V2TIMManager.getFriendshipManager().getFriendApplicationList(
            new V2TIMValueCallback<V2TIMFriendApplicationResult>() {
                @Override public void onSuccess(V2TIMFriendApplicationResult result) {
                    V2TIMFriendApplication match = null;
                    if (result != null && result.getFriendApplicationList() != null) {
                        for (V2TIMFriendApplication application : result.getFriendApplicationList()) {
                            if (application.getType() == V2TIMFriendApplication.V2TIM_FRIEND_APPLICATION_COME_IN
                                && userId.equals(application.getUserID())) { match = application; break; }
                        }
                    }
                    if (match == null) { completion.onError(-1, "Friend request no longer exists"); return; }
                    if (accept) {
                        V2TIMManager.getFriendshipManager().acceptFriendApplication(match,
                            V2TIMFriendApplication.V2TIM_FRIEND_ACCEPT_AGREE_AND_ADD,
                            friendOperation(completion));
                    } else {
                        V2TIMManager.getFriendshipManager().refuseFriendApplication(match,
                            friendOperation(completion));
                    }
                }
                @Override public void onError(int code, String message) {
                    completion.onError(code, message);
                }
            });
    }

    private V2TIMValueCallback<V2TIMFriendOperationResult> friendOperation(OperationCompletion completion) {
        return new V2TIMValueCallback<V2TIMFriendOperationResult>() {
            @Override public void onSuccess(V2TIMFriendOperationResult result) {
                if (result != null && result.getResultCode() != 0) {
                    completion.onError(result.getResultCode(), result.getResultInfo());
                } else { completion.onSuccess(); }
            }
            @Override public void onError(int code, String message) {
                completion.onError(code, message);
            }
        };
    }

    public void deleteContact(String peerId, OperationCompletion completion) {
        if (closed) return;
        if (android.os.Looper.myLooper() != sdkThread.getLooper()) {
            sdkHandler.post(() -> deleteContact(peerId, completion)); return;
        }
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
        if (closed) return;
        if (android.os.Looper.myLooper() != sdkThread.getLooper()) {
            sdkHandler.post(() -> refreshProfiles(userIds)); return;
        }
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
                    // 资料刷新尽力而为，失败不能中断消息收发。
                }
            }
        );
    }

    public void refreshAndSubscribePresence(List<String> userIds) {
        if (closed) return;
        if (android.os.Looper.myLooper() != sdkThread.getLooper()) {
            sdkHandler.post(() -> refreshAndSubscribePresence(userIds)); return;
        }
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

    public void sendActivity(String peerId, RemoteIMActivitySignal signal) {
        if (closed) return;
        if (android.os.Looper.myLooper() != sdkThread.getLooper()) {
            sdkHandler.post(() -> sendActivity(peerId, signal)); return;
        }
        if (currentUserId.isEmpty() || clean(peerId).isEmpty()) return;
        V2TIMMessage message = V2TIMManager.getMessageManager().createCustomMessage(signal.encode());
        if (message == null) return;
        message.setExcludedFromUnreadCount(true);
        message.setExcludedFromLastMessage(true);
        V2TIMManager.getMessageManager().sendMessage(message, clean(peerId), null,
            V2TIMMessage.V2TIM_PRIORITY_DEFAULT, true, null, null);
    }

    private void send(
        V2TIMMessage message,
        String peerId,
        RemoteIMOrigin origin,
        SendCompletion completion
    ) {
        send(message, peerId, RemoteIMProtocolMetadata.encode(origin), completion);
    }

    private void send(
        V2TIMMessage message,
        String peerId,
        String cloudCustomData,
        SendCompletion completion
    ) {
        if (message == null) {
            Log.w(TAG, "send aborted: SDK returned a null message");
            completion.onError(-1, "消息创建失败");
            return;
        }
        message.setCloudCustomData(cloudCustomData);
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

    void handleIncomingMessage(V2TIMMessage sdkMessage) {
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
        if (sdkMessage.getElemType() == V2TIMMessage.V2TIM_ELEM_TYPE_CUSTOM) {
            RemoteIMActivitySignal signal = sdkMessage.getCustomElem() == null ? null
                : RemoteIMActivitySignal.decode(sdkMessage.getCustomElem().getData());
            if (signal != null && clean(sdkMessage.getGroupID()).isEmpty()) {
                listener.onIncomingActivity(fromUserId, currentUserId, signal);
            }
            return;
        }
        long createdAt = sdkMessage.getTimestamp() > 0
            ? sdkMessage.getTimestamp() * 1000L
            : System.currentTimeMillis();
        String recipientUserId = currentUserId;
        String messageId = clean(sdkMessage.getMsgID());
        RemoteIMProtocolMetadata.Metadata metadata =
            RemoteIMProtocolMetadata.decodeMetadata(sdkMessage.getCloudCustomData());
        RemoteIMOrigin origin = metadata.origin();
        IncomingElementParts parts = incomingElementParts(sdkMessage);

        // 先遍历完整消息元素链再决定分支。文字排在前面的多元素消息仍可能有附件；
        // 只看 getTextElem() 就返回会静默丢失该附件。
        if (parts.image != null) {
            downloadImage(
                sdkMessage, parts.image, fromUserId, recipientUserId, createdAt, origin,
                parts.caption, metadata.captionAbove(), metadata.quote()
            );
            return;
        }
        if (parts.sound != null) {
            downloadVoice(
                sdkMessage, parts.sound, fromUserId, recipientUserId, createdAt, origin,
                metadata.quote()
            );
            return;
        }
        if (parts.video != null) {
            downloadVideo(
                sdkMessage, parts.video, fromUserId, recipientUserId, createdAt, origin,
                parts.caption, metadata.captionAbove(), metadata.quote()
            );
            return;
        }
        if (parts.file != null) {
            downloadFile(
                sdkMessage, parts.file, fromUserId, recipientUserId, createdAt, origin,
                parts.caption, metadata.captionAbove(), metadata.quote()
            );
            return;
        }
        if (!parts.caption.isEmpty()) {
            if (metadata.approvalRequest() != null) {
                Log.i(TAG, "approval: request received. from=" + fromUserId
                    + " id=" + messageId
                    + " actions=" + metadata.approvalRequest().actions().size());
            }
            RemoteIMMessage incoming = new RemoteIMMessage(
                null,
                messageId,
                fromUserId,
                recipientUserId,
                parts.caption,
                RemoteIMMessage.Direction.INCOMING,
                RemoteIMMessage.Status.RECEIVED,
                createdAt,
                null,
                null,
                null,
                null,
                origin,
                metadata.approvalRequest(),
                metadata.approvalDecision()
            );
            incoming.setQuote(metadata.quote());
            listener.onIncomingMessage(incoming);
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

    static final class IncomingElementParts {
        String caption = "";
        V2TIMImageElem image;
        V2TIMSoundElem sound;
        V2TIMVideoElem video;
        V2TIMFileElem file;
    }

    private static V2TIMElem firstElement(V2TIMMessage message) {
        switch (message.getElemType()) {
            case V2TIMMessage.V2TIM_ELEM_TYPE_TEXT: return message.getTextElem();
            case V2TIMMessage.V2TIM_ELEM_TYPE_IMAGE: return message.getImageElem();
            case V2TIMMessage.V2TIM_ELEM_TYPE_SOUND: return message.getSoundElem();
            case V2TIMMessage.V2TIM_ELEM_TYPE_VIDEO: return message.getVideoElem();
            case V2TIMMessage.V2TIM_ELEM_TYPE_FILE: return message.getFileElem();
            default: return null;
        }
    }

    static IncomingElementParts incomingElementParts(V2TIMMessage message) {
        IncomingElementParts parts = new IncomingElementParts();
        for (V2TIMElem element = firstElement(message); element != null; element = element.getNextElem()) {
            if (element instanceof V2TIMTextElem && parts.caption.isEmpty()) {
                parts.caption = clean(((V2TIMTextElem) element).getText());
            } else if (element instanceof V2TIMImageElem && parts.image == null) {
                parts.image = (V2TIMImageElem) element;
            } else if (element instanceof V2TIMSoundElem && parts.sound == null) {
                parts.sound = (V2TIMSoundElem) element;
            } else if (element instanceof V2TIMVideoElem && parts.video == null) {
                parts.video = (V2TIMVideoElem) element;
            } else if (element instanceof V2TIMFileElem && parts.file == null) {
                parts.file = (V2TIMFileElem) element;
            }
        }
        return parts;
    }

    private void downloadImage(
        V2TIMMessage message,
        V2TIMImageElem elem,
        String fromUserId,
        String recipientUserId,
        long createdAt,
        RemoteIMOrigin origin,
        String caption,
        boolean captionAbove,
        RemoteIMQuote quote
    ) {
        V2TIMImageElem.V2TIMImage selected = null;
        for (V2TIMImageElem.V2TIMImage image : elem.getImageList()) {
            if (selected == null || imageScore(image) > imageScore(selected)) selected = image;
        }
        if (selected == null) {
            Log.w(TAG, "recv image: elem carries no image entry; dropped id="
                + clean(message.getMsgID()));
            return;
        }
        String mediaId = clean(selected.getUUID());
        if (mediaId.isEmpty()) mediaId = clean(message.getMsgID());
        String messageId = clean(message.getMsgID());
        if (messageId.isEmpty()) messageId = mediaId;
        String imageUrl = selected.getUrl();
        File target = mediaFile(RemoteIMMediaPaths.IMAGES, mediaId, extension(imageUrl, "jpg"));
        V2TIMImageElem.V2TIMImage image = selected;
        String finalMessageId = messageId;
        MaiChatAttachmentDownload.Scope scope = mediaScope;
        downloadMedia(imageUrl, target, MaiChatAttachmentDownload.MAX_IMAGE_BYTES,
            recipientUserId, scope, result -> {
                if (result.ok()) {
                    RemoteIMImageAttachment attachment = new RemoteIMImageAttachment(
                        target.getAbsolutePath(), image.getWidth(), image.getHeight(), image.getSize());
                    RemoteIMMessage incoming = new RemoteIMMessage(
                        null, finalMessageId, fromUserId, recipientUserId,
                        caption.isEmpty() ? "[图片消息] " + target.getName() : caption,
                        RemoteIMMessage.Direction.INCOMING, RemoteIMMessage.Status.RECEIVED,
                        createdAt, attachment, null, null, null, origin);
                    incoming.setCaptionAbove(!caption.isEmpty() && captionAbove);
                    incoming.setQuote(quote);
                    listener.onIncomingMessage(incoming);
                } else {
                    logMediaFailure("image", result);
                }
        });
    }

    private void downloadVoice(
        V2TIMMessage message,
        V2TIMSoundElem elem,
        String fromUserId,
        String recipientUserId,
        long createdAt,
        RemoteIMOrigin origin,
        RemoteIMQuote quote
    ) {
        String mediaId = clean(elem.getUUID());
        if (mediaId.isEmpty()) mediaId = clean(message.getMsgID());
        String messageId = clean(message.getMsgID());
        if (messageId.isEmpty()) messageId = mediaId;
        File target = mediaFile(RemoteIMMediaPaths.VOICES, mediaId, "m4a");
        int duration = Math.max(1, elem.getDuration());
        String finalMessageId = messageId;
        MaiChatAttachmentDownload.Scope scope = mediaScope;
        elem.getUrl(new V2TIMValueCallback<String>() {
            @Override public void onSuccess(String url) {
                if (!isCurrentDownload(recipientUserId, scope)) return;
                downloadMedia(url, target, MaiChatAttachmentDownload.MAX_VOICE_BYTES,
                    recipientUserId, scope, result -> {
                        if (!result.ok()) {
                            logMediaFailure("voice", result);
                            return;
                        }
                        RemoteIMMessage incoming = new RemoteIMMessage(
                            null, finalMessageId, fromUserId, recipientUserId,
                            "[语音消息 " + duration + "s]",
                            RemoteIMMessage.Direction.INCOMING, RemoteIMMessage.Status.RECEIVED,
                            createdAt, null,
                            new RemoteIMVoiceAttachment(target.getAbsolutePath(), duration),
                            null, null, origin);
                        incoming.setQuote(quote);
                        listener.onIncomingMessage(incoming);
                });
            }
            @Override public void onError(int code, String description) {
                if (isCurrentDownload(recipientUserId, scope))
                    Log.w(TAG, "voice URL failed: code=" + code + " " + description);
            }
        });
    }

    private void downloadVideo(
        V2TIMMessage message,
        V2TIMVideoElem elem,
        String fromUserId,
        String recipientUserId,
        long createdAt,
        RemoteIMOrigin origin,
        String caption,
        boolean captionAbove,
        RemoteIMQuote quote
    ) {
        String mediaId = clean(elem.getVideoUUID());
        if (mediaId.isEmpty()) mediaId = clean(message.getMsgID());
        String messageId = clean(message.getMsgID());
        if (messageId.isEmpty()) messageId = mediaId;
        final String finalMessageId = messageId;

        final File videoTarget = mediaFile(RemoteIMMediaPaths.VIDEOS, mediaId, extension(elem.getVideoPath(), "mp4"));
        final File coverTarget = mediaFile(RemoteIMMediaPaths.VIDEO_COVERS, mediaId + "-cover", "jpg");

        final int duration = Math.max(0, (int) elem.getDuration());
        final int width = Math.max(0, elem.getSnapshotWidth());
        final int height = Math.max(0, elem.getSnapshotHeight());
        final long sizeBytes = Math.max(0L, elem.getVideoSize());

        // 视频体积大、下得慢，等它下完再出气泡会让聊天界面长时间什么都不显示。
        // 封面一到就先把消息投出去；视频路径指向最终目标，下完即可播放。
        // 封面失败时退回等视频下完再投，保证消息不会因为没有封面而丢失。
        final AtomicBoolean delivered = new AtomicBoolean(false);
        final AtomicBoolean coverDownloaded = new AtomicBoolean(false);
        MaiChatAttachmentDownload.Scope scope = mediaScope;

        elem.getSnapshotUrl(new V2TIMValueCallback<String>() {
            @Override public void onSuccess(String url) {
                if (!isCurrentDownload(recipientUserId, scope)) return;
                downloadMedia(url, coverTarget, MaiChatAttachmentDownload.MAX_VIDEO_COVER_BYTES,
                    recipientUserId, scope, result -> {
                        if (!result.ok()) {
                            logMediaFailure("video cover", result);
                            return;
                        }
                        coverDownloaded.set(true);
                        emitVideoMessage(finalMessageId, fromUserId, recipientUserId, createdAt, origin,
                            videoTarget.getAbsolutePath(), coverTarget.getAbsolutePath(),
                            duration, width, height, sizeBytes, caption, captionAbove, quote,
                            delivered.compareAndSet(false, true));
                });
            }
            @Override public void onError(int code, String description) {
                if (isCurrentDownload(recipientUserId, scope))
                    Log.w(TAG, "video cover URL failed: code=" + code + " " + description);
            }
        });

        elem.getVideoUrl(new V2TIMValueCallback<String>() {
            @Override public void onSuccess(String url) {
                if (!isCurrentDownload(recipientUserId, scope)) return;
                downloadMedia(url, videoTarget, MaiChatAttachmentDownload.MAX_VIDEO_BYTES,
                    recipientUserId, scope, result -> {
                        if (result.ok()) {
                            emitVideoMessage(finalMessageId, fromUserId, recipientUserId, createdAt, origin,
                                videoTarget.getAbsolutePath(),
                                coverDownloaded.get() ? coverTarget.getAbsolutePath() : "",
                                duration, width, height, sizeBytes, caption, captionAbove, quote,
                                delivered.compareAndSet(false, true));
                        } else {
                            logMediaFailure("video", result);
                        }
                });
            }
            @Override public void onError(int code, String description) {
                if (isCurrentDownload(recipientUserId, scope))
                    Log.w(TAG, "video URL failed: code=" + code + " " + description);
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
        long sizeBytes,
        String caption,
        boolean captionAbove,
        RemoteIMQuote quote,
        boolean initial
    ) {
        RemoteIMVideoAttachment attachment = new RemoteIMVideoAttachment(
            videoPath, coverPath, duration, width, height, sizeBytes
        );
        RemoteIMMessage incoming = new RemoteIMMessage(
            null,
            remoteId,
            fromUserId,
            recipientUserId,
            caption.isEmpty() ? "[视频消息 " + duration + "s]" : caption,
            RemoteIMMessage.Direction.INCOMING,
            RemoteIMMessage.Status.RECEIVED,
            createdAt,
            null,
            null,
            null,
            attachment,
            origin
        );
        incoming.setCaptionAbove(!caption.isEmpty() && captionAbove);
        incoming.setQuote(quote);
        if (initial) listener.onIncomingMessage(incoming);
        else listener.onVideoMediaUpdated(fromUserId, recipientUserId, remoteId, attachment);
    }

    private void downloadFile(
        V2TIMMessage message,
        V2TIMFileElem elem,
        String fromUserId,
        String recipientUserId,
        long createdAt,
        RemoteIMOrigin origin,
        String caption,
        boolean captionAbove,
        RemoteIMQuote quote
    ) {
        String mediaId = clean(elem.getUUID());
        if (mediaId.isEmpty()) mediaId = clean(message.getMsgID());
        String messageId = clean(message.getMsgID());
        if (messageId.isEmpty()) messageId = mediaId;
        String fileName = safeFileName(elem.getFileName(), "remote-im-file.bin");
        File target = mediaFile(RemoteIMMediaPaths.FILES, mediaId, extension(fileName, "bin"));
        String finalMessageId = messageId;
        MaiChatAttachmentDownload.Scope scope = mediaScope;
        elem.getUrl(new V2TIMValueCallback<String>() {
            @Override public void onSuccess(String url) {
                if (!isCurrentDownload(recipientUserId, scope)) return;
                downloadMedia(url, target, MaiChatAttachmentDownload.MAX_FILE_BYTES,
                    recipientUserId, scope, result -> {
                        if (!result.ok()) {
                            logMediaFailure("file", result);
                            return;
                        }
                        RemoteIMFileAttachment attachment = new RemoteIMFileAttachment(
                            target.getAbsolutePath(), fileName, mimeType(fileName), elem.getFileSize());
                        RemoteIMMessage incoming = new RemoteIMMessage(
                            null, finalMessageId, fromUserId, recipientUserId,
                            caption.isEmpty() ? "[文件消息] " + fileName : caption,
                            RemoteIMMessage.Direction.INCOMING, RemoteIMMessage.Status.RECEIVED,
                            createdAt, null, null, attachment, null, origin);
                        incoming.setCaptionAbove(!caption.isEmpty() && captionAbove);
                        incoming.setQuote(quote);
                        listener.onIncomingMessage(incoming);
                });
            }
            @Override public void onError(int code, String description) {
                if (isCurrentDownload(recipientUserId, scope))
                    Log.w(TAG, "file URL failed: code=" + code + " " + description);
            }
        });
    }

    private boolean isCurrentDownload(String account, MaiChatAttachmentDownload.Scope scope) {
        return !closed && mediaScope == scope && !scope.isCanceled()
            && account.equals(currentUserId);
    }

    private void downloadMedia(String url, File target, long maximumBytes, String account,
                               MaiChatAttachmentDownload.Scope scope,
                               MaiChatAttachmentDownload.Completion completion) {
        if (!isCurrentDownload(account, scope)) return;
        mediaDownloader.download(url, target, maximumBytes, scope, result ->
            sdkHandler.post(() -> {
                if (isCurrentDownload(account, scope)) completion.finished(result);
            }));
    }

    private static void logMediaFailure(String kind, MaiChatAttachmentDownload.Result result) {
        if (result.status == MaiChatAttachmentDownload.CANCELED) return;
        Log.w(TAG, kind + " attachment download failed: status=" + result.status
            + " http=" + result.httpStatus + " curl=" + result.curlCode);
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
