package com.kongshang.maichat;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.nio.charset.StandardCharsets;

public final class RemoteInputPacket {
    public static final int UNRELIABLE_COMMAND_ID = 2;
    public static final int RELIABLE_COMMAND_ID = 3;
    public static final int MAXIMUM_PACKET_BYTES = 1024;

    private RemoteInputPacket() {
    }

    public static JSONObject move(double x, double y) {
        return pointEvent("m", x, y);
    }

    public static JSONObject button(int button, boolean pressed, double x, double y) {
        JSONObject event = pointEvent("b", x, y);
        try {
            event.put("b", button);
            event.put("d", pressed);
            return event;
        } catch (JSONException error) {
            throw new IllegalStateException(error);
        }
    }

    public static JSONObject wheel(int delta, double x, double y) {
        JSONObject event = pointEvent("w", x, y);
        try {
            event.put("w", delta);
            return event;
        } catch (JSONException error) {
            throw new IllegalStateException(error);
        }
    }

    public static JSONObject key(int code, boolean pressed) {
        try {
            return new JSONObject().put("t", "k").put("k", code).put("d", pressed);
        } catch (JSONException error) {
            throw new IllegalStateException(error);
        }
    }

    public static JSONObject text(String text) {
        try {
            return new JSONObject().put("t", "x").put("s", text == null ? "" : text);
        } catch (JSONException error) {
            throw new IllegalStateException(error);
        }
    }

    public static JSONObject releaseAll() {
        try {
            return new JSONObject().put("t", "r");
        } catch (JSONException error) {
            throw new IllegalStateException(error);
        }
    }

    public static byte[] encode(String sessionId, long sequence, JSONObject... events) {
        try {
            JSONArray array = new JSONArray();
            for (JSONObject event : events) array.put(event);
            JSONObject packet = new JSONObject()
                .put("v", 1)
                .put("s", sessionId)
                .put("n", sequence)
                .put("e", array);
            return packet.toString().getBytes(StandardCharsets.UTF_8);
        } catch (JSONException error) {
            throw new IllegalStateException("远程输入编码失败", error);
        }
    }

    private static JSONObject pointEvent(String type, double x, double y) {
        try {
            return new JSONObject()
                .put("t", type)
                .put("x", clamp(x))
                .put("y", clamp(y));
        } catch (JSONException error) {
            throw new IllegalStateException(error);
        }
    }

    private static double clamp(double value) {
        if (!Double.isFinite(value)) return 0;
        return Math.min(1, Math.max(0, value));
    }
}
