// 文件作用：Android 本地测试；BroadcastDeliveryTrackerTest 负责覆盖对应类的状态和数据行为，本文件给出其验证。
// 运行边界：仅供自动化验证使用，不作为正式 App 或 Agent 的用户功能入口。
// 覆盖场景：reportsAllFailedRecipientsOnceAfterEveryReceiptArrives。
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
