package com.kongshang.maichat;

import org.json.JSONObject;
import java.nio.charset.StandardCharsets;

/** Versioned, online-only presentation signal shared with iOS and Desktop. */
public final class RemoteIMActivitySignal {
    public enum Kind {
        HUMAN_TYPING("human-typing", ""), MACHINE_WORKING("machine-working", "正在执行…"),
        MACHINE_THINKING("machine-thinking", "思考中…"), MACHINE_TOOL("machine-tool", "正在使用工具…"),
        MACHINE_WAITING("machine-waiting", "等待确认…");
        public final String wireName;
        public final String label;
        Kind(String wireName, String label) { this.wireName = wireName; this.label = label; }
    }
    public final String activityId;
    public final long sequence;
    public final Kind kind;
    public final boolean active;
    public final long startedAtMs;
    public final long taskStartedAtMs;
    public final int ttlMs;

    public RemoteIMActivitySignal(String id, long sequence, Kind kind, boolean active, int ttlMs) {
        this(id, sequence, kind, active, ttlMs, System.currentTimeMillis());
    }

    private RemoteIMActivitySignal(String id, long sequence, Kind kind, boolean active, int ttlMs,
                                   long nowMs) {
        this(id, sequence, kind, active, ttlMs, nowMs, nowMs);
    }

    public RemoteIMActivitySignal(String id, long sequence, Kind kind, boolean active, int ttlMs,
                                  long startedAtMs, long taskStartedAtMs) {
        if (id == null || !id.matches("[A-Za-z0-9._:-]{1,192}") || kind == null
            || sequence < 0 || sequence > 9_007_199_254_740_991L || startedAtMs <= 0
            || taskStartedAtMs <= 0 || taskStartedAtMs > startedAtMs) {
            throw new IllegalArgumentException("Invalid activity");
        }
        this.activityId = id; this.sequence = sequence; this.kind = kind; this.active = active;
        this.startedAtMs = startedAtMs; this.taskStartedAtMs = taskStartedAtMs;
        this.ttlMs = Math.max(1000, Math.min(30000, ttlMs));
    }

    public byte[] encode() {
        try {
            return new JSONObject().put("namespace", "multi-ai-code-activity").put("version", 2)
                .put("activityId", activityId).put("sequence", sequence).put("kind", kind.wireName)
                .put("active", active).put("startedAtMs", startedAtMs)
                .put("taskStartedAtMs", taskStartedAtMs).put("ttlMs", ttlMs)
                .toString().getBytes(StandardCharsets.UTF_8);
        } catch (Exception error) { throw new IllegalStateException(error); }
    }

    public static RemoteIMActivitySignal decode(byte[] data) {
        if (data == null || data.length > 2048) return null;
        try {
            JSONObject value = new JSONObject(new String(data, StandardCharsets.UTF_8));
            if (!"multi-ai-code-activity".equals(value.opt("namespace"))
                || !(value.opt("version") instanceof Number) || value.getDouble("version") != 2
                || !(value.opt("activityId") instanceof String) || !(value.opt("kind") instanceof String)
                || !(value.opt("active") instanceof Boolean) || !(value.opt("sequence") instanceof Number)
                || !(value.opt("startedAtMs") instanceof Number)
                || !(value.opt("taskStartedAtMs") instanceof Number)
                || !(value.opt("ttlMs") instanceof Number)) return null;
            double sequence = value.getDouble("sequence"), ttl = value.getDouble("ttlMs");
            double startedAt = value.getDouble("startedAtMs");
            double taskStartedAt = value.getDouble("taskStartedAtMs");
            if (!Double.isFinite(sequence) || sequence != Math.floor(sequence)
                || sequence < 0 || sequence > 9_007_199_254_740_991L || !Double.isFinite(ttl)
                || !Double.isFinite(startedAt) || startedAt != Math.floor(startedAt)
                || startedAt <= 0 || startedAt > 9_007_199_254_740_991L
                || !Double.isFinite(taskStartedAt) || taskStartedAt != Math.floor(taskStartedAt)
                || taskStartedAt <= 0 || taskStartedAt > startedAt) return null;
            for (Kind kind : Kind.values()) if (kind.wireName.equals(value.getString("kind"))) {
                return new RemoteIMActivitySignal(value.getString("activityId"), (long) sequence,
                    kind, value.getBoolean("active"), (int) Math.max(1000, Math.min(30000, ttl)),
                    (long) startedAt, (long) taskStartedAt);
            }
        } catch (Exception ignored) { }
        return null;
    }
}
