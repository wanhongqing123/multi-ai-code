package com.kongshang.maichat;

import org.json.JSONException;
import org.json.JSONObject;

public final class RemoteDesktopSignal {
    public enum Kind {
        INVITE("invite"),
        ACCEPT("accept"),
        REJECT("reject"),
        STOP("stop"),
        NOTICE("notice");

        private final String wireValue;

        Kind(String wireValue) {
            this.wireValue = wireValue;
        }

        static Kind fromWireValue(String value) {
            for (Kind kind : values()) {
                if (kind.wireValue.equals(value)) return kind;
            }
            return null;
        }
    }

    public static final String PREFIX = "\u2063\u200B[remote-desktop]";
    public static final int VERSION = 1;

    public static final class CaptureGeometry {
        public final int sourceWidth;
        public final int sourceHeight;
        public final int captureX;
        public final int captureY;
        public final int captureWidth;
        public final int captureHeight;
        public final int revision;

        CaptureGeometry(JSONObject object) {
            sourceWidth = object.optInt("sourceWidth");
            sourceHeight = object.optInt("sourceHeight");
            captureX = object.optInt("captureX");
            captureY = object.optInt("captureY");
            captureWidth = object.optInt("captureWidth");
            captureHeight = object.optInt("captureHeight");
            revision = object.optInt("revision");
        }

        public boolean isValid() {
            return sourceWidth > 0
                && sourceHeight > 0
                && captureWidth > 0
                && captureHeight > 0
                && captureX >= 0
                && captureY >= 0
                && captureX + captureWidth <= sourceWidth
                && captureY + captureHeight <= sourceHeight
                && revision > 0;
        }
    }

    public final Kind kind;
    public final String sessionId;
    public final String roomId;
    public final String reason;
    public final String noticeCode;
    public final CaptureGeometry captureGeometry;

    private RemoteDesktopSignal(
        Kind kind,
        String sessionId,
        String roomId,
        String reason,
        String noticeCode,
        CaptureGeometry captureGeometry
    ) {
        this.kind = kind;
        this.sessionId = clean(sessionId);
        this.roomId = clean(roomId);
        this.reason = clean(reason);
        this.noticeCode = clean(noticeCode);
        this.captureGeometry = captureGeometry != null && captureGeometry.isValid()
            ? captureGeometry
            : null;
    }

    public static RemoteDesktopSignal create(Kind kind, String sessionId, String roomId) {
        return new RemoteDesktopSignal(kind, sessionId, roomId, "", "", null);
    }

    public static RemoteDesktopSignal createReject(String sessionId, String reason) {
        return new RemoteDesktopSignal(Kind.REJECT, sessionId, "", reason, "", null);
    }

    public String encode() {
        try {
            JSONObject payload = new JSONObject();
            payload.put("v", VERSION);
            payload.put("type", kind.wireValue);
            if (!sessionId.isEmpty()) payload.put("sessionId", sessionId);
            if (!roomId.isEmpty()) payload.put("roomId", roomId);
            if (!reason.isEmpty()) payload.put("reason", reason);
            if (!noticeCode.isEmpty()) payload.put("noticeCode", noticeCode);
            return PREFIX + payload.toString();
        } catch (JSONException error) {
            throw new IllegalStateException("远程桌面信令编码失败", error);
        }
    }

    public static boolean isSignal(String text) {
        return text != null && text.startsWith(PREFIX);
    }

    public static RemoteDesktopSignal decode(String text) {
        if (!isSignal(text)) return null;
        try {
            JSONObject payload = new JSONObject(text.substring(PREFIX.length()));
            if (payload.optInt("v") != VERSION) return null;
            Kind kind = Kind.fromWireValue(payload.optString("type"));
            if (kind == null) return null;
            CaptureGeometry geometry = payload.has("captureGeometry")
                ? new CaptureGeometry(payload.getJSONObject("captureGeometry"))
                : null;
            return new RemoteDesktopSignal(
                kind,
                payload.optString("sessionId"),
                payload.optString("roomId"),
                payload.optString("reason"),
                payload.optString("noticeCode"),
                geometry
            );
        } catch (JSONException error) {
            return null;
        }
    }

    private static String clean(String value) {
        return value == null ? "" : value.trim();
    }
}
