package com.kongshang.maichat;

import android.content.Context;
import android.os.Handler;
import android.os.Looper;

import com.tencent.rtmp.ui.TXCloudVideoView;
import com.tencent.trtc.TRTCCloud;
import com.tencent.trtc.TRTCCloudDef;
import com.tencent.trtc.TRTCCloudListener;

import java.util.UUID;
import java.util.Locale;

public final class RemoteDesktopController {
    public enum State {
        IDLE,
        INVITING,
        CONNECTING,
        VIEWING,
        FAILED
    }

    public interface Listener {
        void onRemoteDesktopChanged();
    }

    private final TencentIMClient imClient;
    private final Listener listener;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final TRTCCloud cloud;
    private final TRTCCloudListener cloudListener = new TRTCCloudListener() {
        @Override
        public void onEnterRoom(long result) {
            if (result < 0) {
                fail("远程连接失败（" + result + "）");
                return;
            }
            enteredRoom = true;
            startRemoteViewIfReady();
        }

        @Override
        public void onUserSubStreamAvailable(String userId, boolean available) {
            if (!peerUserId.equals(userId)) return;
            substreamAvailable = available;
            if (available) {
                startRemoteViewIfReady();
            } else if (state == State.VIEWING) {
                state = State.CONNECTING;
                notifyChanged();
            }
        }

        @Override
        public void onFirstVideoFrame(String userId, int streamType, int width, int height) {
            if (!peerUserId.equals(userId) || streamType != TRTCCloudDef.TRTC_VIDEO_STREAM_TYPE_SUB) {
                return;
            }
            remoteVideoWidth = width;
            remoteVideoHeight = height;
            state = State.VIEWING;
            notifyChanged();
        }

        @Override
        public void onUserVideoSizeChanged(String userId, int streamType, int width, int height) {
            if (!peerUserId.equals(userId) || streamType != TRTCCloudDef.TRTC_VIDEO_STREAM_TYPE_SUB) {
                return;
            }
            remoteVideoWidth = width;
            remoteVideoHeight = height;
            notifyChanged();
        }

        @Override
        public void onError(int code, String message, android.os.Bundle extraInfo) {
            fail(message == null || message.trim().isEmpty()
                ? "远程连接失败（" + code + "）"
                : message);
        }

        @Override
        public void onConnectionLost() {
            notice = "远程连接已中断，正在重连";
            notifyChanged();
        }

        @Override
        public void onConnectionRecovery() {
            notice = "";
            notifyChanged();
        }
    };

    private State state = State.IDLE;
    private String peerUserId = "";
    private String sessionId = "";
    private String roomId = "";
    private String localUserId = "";
    private String userSig = "";
    private String error = "";
    private String notice = "";
    private boolean controlEnabled;
    private boolean leftButtonHeld;
    private boolean enteredRoom;
    private boolean substreamAvailable;
    private long reliableSequence;
    private long unreliableSequence;
    private int remoteVideoWidth;
    private int remoteVideoHeight;
    private TXCloudVideoView renderView;
    private RemoteDesktopSignal.CaptureGeometry captureGeometry;
    private double lastPointerX = 0.5;
    private double lastPointerY = 0.5;
    private final Runnable invitationTimeout = () -> {
        if (state != State.INVITING && state != State.CONNECTING) return;
        String peer = peerUserId;
        String trace = sessionId;
        fail("对方未在规定时间内响应");
        sendSignal(RemoteDesktopSignal.create(RemoteDesktopSignal.Kind.STOP, trace, ""), peer);
    };

    public RemoteDesktopController(Context context, TencentIMClient imClient, Listener listener) {
        this.imClient = imClient;
        this.listener = listener;
        cloud = TRTCCloud.sharedInstance(context.getApplicationContext());
        cloud.addListener(cloudListener);
    }

    public State state() {
        return state;
    }

    public String peerUserId() {
        return peerUserId;
    }

    public String error() {
        return error;
    }

    public String notice() {
        return notice;
    }

    public boolean isControlEnabled() {
        return controlEnabled;
    }

    public boolean isLeftButtonHeld() {
        return leftButtonHeld;
    }

    public boolean isActive() {
        return state == State.INVITING || state == State.CONNECTING || state == State.VIEWING;
    }

    public String statusText() {
        switch (state) {
            case INVITING: return "等待对方确认...";
            case CONNECTING: return "正在连接对方屏幕...";
            case VIEWING: return "远程画面已连接";
            case FAILED: return error;
            case IDLE:
            default: return "没有进行中的远程桌面";
        }
    }

    public void requestView(String peerId, String localUserId, String userSig) {
        if (isActive() || peerId == null || peerId.trim().isEmpty()) return;
        reset(false);
        this.peerUserId = peerId.trim();
        this.localUserId = localUserId.trim();
        this.userSig = userSig;
        sessionId = UUID.randomUUID().toString().replace("-", "").toLowerCase(Locale.ROOT);
        roomId = "mc-" + this.localUserId + "-"
            + UUID.randomUUID().toString().replace("-", "").toLowerCase(Locale.ROOT);
        state = State.INVITING;
        notifyChanged();
        handler.postDelayed(invitationTimeout, 30_000);
        sendSignal(
            RemoteDesktopSignal.create(RemoteDesktopSignal.Kind.INVITE, sessionId, roomId),
            this.peerUserId
        );
    }

    public boolean handleIncomingText(String fromUserId, String text) {
        if (!RemoteDesktopSignal.isSignal(text)) return false;
        RemoteDesktopSignal signal = RemoteDesktopSignal.decode(text);
        if (signal == null) return true;
        switch (signal.kind) {
            case INVITE:
                sendSignal(
                    RemoteDesktopSignal.createReject(signal.sessionId, "Android 暂不支持共享本机屏幕"),
                    fromUserId
                );
                break;
            case ACCEPT:
                if (!matches(fromUserId, signal.sessionId) || state != State.INVITING) break;
                handler.removeCallbacks(invitationTimeout);
                if (!signal.roomId.isEmpty()) roomId = signal.roomId;
                captureGeometry = signal.captureGeometry;
                state = State.CONNECTING;
                notifyChanged();
                enterRoom();
                break;
            case REJECT:
                if (!matches(fromUserId, signal.sessionId)) break;
                handler.removeCallbacks(invitationTimeout);
                fail(signal.reason.isEmpty() ? "对方拒绝了请求" : signal.reason);
                break;
            case STOP:
                if (matches(fromUserId, signal.sessionId)) reset(true);
                break;
            case NOTICE:
                if (!matches(fromUserId, signal.sessionId)) break;
                if ("secure-desktop-entered".equals(signal.noticeCode)) {
                    notice = "对方进入了安全桌面，画面可能暂时不可用";
                } else if ("secure-desktop-left".equals(signal.noticeCode)) {
                    notice = "";
                }
                notifyChanged();
                break;
        }
        return true;
    }

    public void bindRenderView(TXCloudVideoView view) {
        renderView = view;
        if (view != null) view.setBackgroundColor(0xFF000000);
        startRemoteViewIfReady();
    }

    public void setControlEnabled(boolean enabled) {
        if (state != State.VIEWING) return;
        if (controlEnabled && !enabled) sendReliable(RemoteInputPacket.releaseAll());
        controlEnabled = enabled;
        if (!enabled) leftButtonHeld = false;
        notifyChanged();
    }

    public void move(double x, double y) {
        if (!controlEnabled) return;
        lastPointerX = clamp(x);
        lastPointerY = clamp(y);
        sendUnreliable(RemoteInputPacket.move(x, y));
    }

    public void click(int button, double x, double y) {
        if (!controlEnabled) return;
        lastPointerX = clamp(x);
        lastPointerY = clamp(y);
        sendReliable(RemoteInputPacket.button(button, true, x, y));
        sendReliable(RemoteInputPacket.button(button, false, x, y));
    }

    public void toggleLeftButtonHeld() {
        if (!controlEnabled) return;
        leftButtonHeld = !leftButtonHeld;
        sendReliable(RemoteInputPacket.button(0, leftButtonHeld, lastPointerX, lastPointerY));
        notifyChanged();
    }

    public void scroll(int delta, double x, double y) {
        if (controlEnabled) sendReliable(RemoteInputPacket.wheel(delta, x, y));
    }

    public void sendText(String text) {
        if (!controlEnabled || text == null || text.isEmpty()) return;
        StringBuilder chunk = new StringBuilder();
        int bytes = 0;
        for (int offset = 0; offset < text.length();) {
            int codePoint = text.codePointAt(offset);
            String value = new String(Character.toChars(codePoint));
            int nextBytes = value.getBytes(java.nio.charset.StandardCharsets.UTF_8).length;
            if (bytes + nextBytes > 700 && chunk.length() > 0) {
                sendReliable(RemoteInputPacket.text(chunk.toString()));
                chunk.setLength(0);
                bytes = 0;
            }
            chunk.append(value);
            bytes += nextBytes;
            offset += Character.charCount(codePoint);
        }
        if (chunk.length() > 0) sendReliable(RemoteInputPacket.text(chunk.toString()));
    }

    public void sendKey(int keyCode) {
        if (!controlEnabled) return;
        sendReliable(RemoteInputPacket.key(keyCode, true));
        sendReliable(RemoteInputPacket.key(keyCode, false));
    }

    public double[] mapPoint(float x, float y, int viewWidth, int viewHeight) {
        if (viewWidth <= 0 || viewHeight <= 0) return null;
        double contentWidth = remoteVideoWidth > 0 ? remoteVideoWidth : viewWidth;
        double contentHeight = remoteVideoHeight > 0 ? remoteVideoHeight : viewHeight;
        if (captureGeometry != null) {
            contentWidth = captureGeometry.captureWidth;
            contentHeight = captureGeometry.captureHeight;
        }
        double scale = Math.min(viewWidth / contentWidth, viewHeight / contentHeight);
        double renderedWidth = contentWidth * scale;
        double renderedHeight = contentHeight * scale;
        double left = (viewWidth - renderedWidth) / 2.0;
        double top = (viewHeight - renderedHeight) / 2.0;
        if (x < left || x > left + renderedWidth || y < top || y > top + renderedHeight) {
            return null;
        }
        double localX = clamp((x - left) / renderedWidth);
        double localY = clamp((y - top) / renderedHeight);
        if (captureGeometry == null) return new double[]{localX, localY};
        return new double[]{
            clamp((captureGeometry.captureX + localX * captureGeometry.captureWidth)
                / captureGeometry.sourceWidth),
            clamp((captureGeometry.captureY + localY * captureGeometry.captureHeight)
                / captureGeometry.sourceHeight)
        };
    }

    public void stop() {
        String peer = peerUserId;
        String trace = sessionId;
        reset(true);
        if (!peer.isEmpty() && !trace.isEmpty()) {
            sendSignal(RemoteDesktopSignal.create(RemoteDesktopSignal.Kind.STOP, trace, ""), peer);
        }
    }

    public void destroy() {
        reset(false);
        cloud.removeListener(cloudListener);
    }

    private void enterRoom() {
        TRTCCloudDef.TRTCParams params = new TRTCCloudDef.TRTCParams();
        params.sdkAppId = RemoteIMSessionController.SDK_APP_ID;
        params.userId = localUserId;
        params.userSig = userSig;
        params.strRoomId = roomId;
        cloud.setDefaultStreamRecvMode(false, false);
        cloud.muteAllRemoteAudio(true);
        cloud.enterRoom(params, TRTCCloudDef.TRTC_APP_SCENE_VIDEOCALL);
    }

    private void startRemoteViewIfReady() {
        if (!enteredRoom || !substreamAvailable || renderView == null || peerUserId.isEmpty()) return;
        TRTCCloudDef.TRTCRenderParams params = new TRTCCloudDef.TRTCRenderParams();
        params.fillMode = TRTCCloudDef.TRTC_VIDEO_RENDER_MODE_FIT;
        cloud.setRemoteRenderParams(peerUserId, TRTCCloudDef.TRTC_VIDEO_STREAM_TYPE_SUB, params);
        cloud.startRemoteView(peerUserId, TRTCCloudDef.TRTC_VIDEO_STREAM_TYPE_SUB, renderView);
    }

    private void sendSignal(RemoteDesktopSignal signal, String peerId) {
        if (peerId == null || peerId.trim().isEmpty()) return;
        imClient.sendText(peerId, signal.encode(), RemoteIMOrigin.MACHINE, new TencentIMClient.SendCompletion() {
            @Override
            public void onSuccess(String remoteId, long createdAtMillis) {
            }

            @Override
            public void onError(int code, String message) {
                if (signal.kind == RemoteDesktopSignal.Kind.INVITE) {
                    fail("远程请求发送失败：" + message);
                }
            }
        });
    }

    private boolean sendReliable(org.json.JSONObject event) {
        return sendEvent(event, true);
    }

    private boolean sendUnreliable(org.json.JSONObject event) {
        return sendEvent(event, false);
    }

    private boolean sendEvent(org.json.JSONObject event, boolean reliable) {
        if (!enteredRoom || state != State.VIEWING) return false;
        long sequence = reliable ? ++reliableSequence : ++unreliableSequence;
        byte[] data = RemoteInputPacket.encode(sessionId, sequence, event);
        if (data.length > RemoteInputPacket.MAXIMUM_PACKET_BYTES) return false;
        int commandId = reliable
            ? RemoteInputPacket.RELIABLE_COMMAND_ID
            : RemoteInputPacket.UNRELIABLE_COMMAND_ID;
        return cloud.sendCustomCmdMsg(commandId, data, reliable, reliable);
    }

    private void fail(String reason) {
        error = reason == null ? "远程连接失败" : reason;
        state = State.FAILED;
        cleanupRoom();
        notifyChanged();
    }

    private void reset(boolean notify) {
        handler.removeCallbacks(invitationTimeout);
        cleanupRoom();
        state = State.IDLE;
        peerUserId = "";
        sessionId = "";
        roomId = "";
        error = "";
        notice = "";
        captureGeometry = null;
        remoteVideoWidth = 0;
        remoteVideoHeight = 0;
        reliableSequence = 0;
        unreliableSequence = 0;
        lastPointerX = 0.5;
        lastPointerY = 0.5;
        if (notify) notifyChanged();
    }

    private void cleanupRoom() {
        if (controlEnabled && enteredRoom) sendReliable(RemoteInputPacket.releaseAll());
        controlEnabled = false;
        leftButtonHeld = false;
        if (!peerUserId.isEmpty()) {
            cloud.stopRemoteView(peerUserId, TRTCCloudDef.TRTC_VIDEO_STREAM_TYPE_SUB);
        }
        if (enteredRoom) cloud.exitRoom();
        enteredRoom = false;
        substreamAvailable = false;
    }

    private boolean matches(String fromUserId, String incomingSessionId) {
        return peerUserId.equals(fromUserId) && sessionId.equals(incomingSessionId);
    }

    private void notifyChanged() {
        handler.post(listener::onRemoteDesktopChanged);
    }

    private static double clamp(double value) {
        if (!Double.isFinite(value)) return 0;
        return Math.min(1, Math.max(0, value));
    }
}
