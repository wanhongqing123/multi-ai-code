package com.kongshang.maichat;

import org.junit.Test;
import static org.junit.Assert.*;
import java.nio.charset.StandardCharsets;

public class RemoteIMActivityTest {
    private RemoteIMActivitySignal signal(String id, long sequence, RemoteIMActivitySignal.Kind kind, boolean active) {
        return new RemoteIMActivitySignal(id, sequence, kind, active, 12000);
    }
    @Test public void decodesTheDesktopAndIosWireFormat() {
        String json = "{\"namespace\":\"multi-ai-code-activity\",\"version\":1,\"activityId\":\"machine:test\",\"sequence\":3,\"kind\":\"machine-tool\",\"active\":true,\"ttlMs\":12000}";
        RemoteIMActivitySignal value = RemoteIMActivitySignal.decode(json.getBytes(StandardCharsets.UTF_8));
        assertNotNull(value); assertEquals(RemoteIMActivitySignal.Kind.MACHINE_TOOL, value.kind);
        assertEquals(3, value.sequence); assertEquals("machine:test", value.activityId);
        assertEquals(value.kind, RemoteIMActivitySignal.decode(value.encode()).kind);
        assertNull(RemoteIMActivitySignal.decode(json.replace("\"active\":true", "\"active\":\"true\"").getBytes(StandardCharsets.UTF_8)));
        assertNull(RemoteIMActivitySignal.decode(json.replace("\"sequence\":3", "\"sequence\":3.5").getBytes(StandardCharsets.UTF_8)));
        assertNull(RemoteIMActivitySignal.decode(json.replace("machine:test", "bad id").getBytes(StandardCharsets.UTF_8)));
    }
    @Test public void heartbeatsDoNotRefreshUiAndAnOldStopCannotRemoveANewLease() {
        RemoteIMActivityState state = new RemoteIMActivityState();
        assertTrue(state.receive("peer", signal("first", 1, RemoteIMActivitySignal.Kind.HUMAN_TYPING, true), 0));
        assertFalse(state.receive("peer", signal("first", 2, RemoteIMActivitySignal.Kind.HUMAN_TYPING, true), 5000));
        assertFalse(state.receive("peer", signal("first", 1, RemoteIMActivitySignal.Kind.MACHINE_TOOL, true), 6000));
        assertTrue(state.receive("peer", signal("second", 1, RemoteIMActivitySignal.Kind.MACHINE_TOOL, true), 7000));
        assertFalse(state.receive("peer", signal("first", 9, RemoteIMActivitySignal.Kind.HUMAN_TYPING, false), 8000));
        assertEquals("second", state.get("peer").activityId);
        assertTrue(state.expire(18000).isEmpty());
        assertTrue(state.expire(19000).contains("peer"));
        assertNull(state.get("peer"));
        assertTrue(state.receive("peer", signal("second", 2, RemoteIMActivitySignal.Kind.MACHINE_TOOL, true), 20000));
    }
    @Test public void contentClosesTheBurstButTheNextStageAndAccountCanStartAgain() {
        RemoteIMActivityState state = new RemoteIMActivityState();
        state.receive("peer", signal("burst", 1, RemoteIMActivitySignal.Kind.MACHINE_THINKING, true), 0);
        assertTrue(state.clear("peer"));
        assertFalse(state.receive("peer", signal("burst", 2, RemoteIMActivitySignal.Kind.MACHINE_THINKING, true), 1000));
        assertTrue(state.receive("peer", signal("next", 1, RemoteIMActivitySignal.Kind.MACHINE_TOOL, true), 2000));
        state.reset(); assertNull(state.get("peer"));
        assertTrue(state.receive("peer", signal("burst", 1, RemoteIMActivitySignal.Kind.MACHINE_THINKING, true), 3000));
    }
}
