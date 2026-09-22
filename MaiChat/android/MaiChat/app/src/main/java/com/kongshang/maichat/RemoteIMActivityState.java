package com.kongshang.maichat;

import java.util.HashMap;
import java.util.LinkedHashSet;
import java.util.Map;
import java.util.Set;

/** UI-thread-owned leases, deliberately separate from ChatState and persistence. */
final class RemoteIMActivityState {
    private final Map<String, RemoteIMActivitySignal> signals = new HashMap<>();
    private final Map<String, Long> expiry = new HashMap<>();
    private final Map<String, Long> sequences = new HashMap<>();
    private final Set<String> closed = new LinkedHashSet<>();
    RemoteIMActivitySignal get(String peer) { return signals.get(peer); }
    boolean receive(String peer, RemoteIMActivitySignal signal, long now) {
        String identity = peer + "\0" + signal.activityId;
        if (closed.contains(identity)) return false;
        RemoteIMActivitySignal old = signals.get(peer);
        if (!signal.active) {
            close(identity);
            return old != null && old.activityId.equals(signal.activityId) && clear(peer);
        }
        if (signal.sequence <= sequences.getOrDefault(identity, -1L)) return false;
        if (sequences.size() >= 512) sequences.clear();
        sequences.put(identity, signal.sequence);
        if (old != null && !old.activityId.equals(signal.activityId)) close(peer + "\0" + old.activityId);
        signals.put(peer, signal);
        expiry.put(peer, now + signal.ttlMs);
        return old == null || !old.activityId.equals(signal.activityId) || old.kind != signal.kind;
    }
    boolean clear(String peer) {
        RemoteIMActivitySignal old = signals.remove(peer);
        expiry.remove(peer);
        if (old != null) close(peer + "\0" + old.activityId);
        return old != null;
    }
    Set<String> expire(long now) {
        Set<String> changed = new LinkedHashSet<>();
        for (Map.Entry<String, Long> entry : expiry.entrySet()) if (entry.getValue() <= now) changed.add(entry.getKey());
        for (String peer : changed) { signals.remove(peer); expiry.remove(peer); }
        return changed;
    }
    long nextExpiry() {
        long next = Long.MAX_VALUE;
        for (long value : expiry.values()) next = Math.min(next, value);
        return next;
    }
    void reset() { signals.clear(); expiry.clear(); sequences.clear(); closed.clear(); }
    private void close(String identity) {
        closed.add(identity);
        if (closed.size() > 256) closed.remove(closed.iterator().next());
    }
}
