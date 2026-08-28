package com.kongshang.maichat;

import static org.junit.Assert.assertEquals;

import org.junit.Test;

import java.util.ArrayList;
import java.util.List;

public class BroadcastDeliveryTrackerTest {
    @Test
    public void reportsAllFailedRecipientsOnceAfterEveryReceiptArrives() {
        List<String> events = new ArrayList<>();
        BroadcastDeliveryTracker tracker = new BroadcastDeliveryTracker(
            3,
            (total, failed) -> events.add(total + ":" + String.join(",", failed))
        );

        tracker.record("alice", false);
        tracker.record("bob", true);
        assertEquals(List.of(), events);
        tracker.record("carol", false);
        tracker.record("late-duplicate", false);

        assertEquals(List.of("3:alice,carol"), events);
    }
}
