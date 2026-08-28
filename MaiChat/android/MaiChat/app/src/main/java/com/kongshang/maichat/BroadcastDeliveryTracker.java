package com.kongshang.maichat;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

final class BroadcastDeliveryTracker {
    interface Completion {
        void onFinished(int total, List<String> failedUserIds);
    }

    private final int total;
    private final Completion completion;
    private final List<String> failed = new ArrayList<>();
    private int remaining;

    BroadcastDeliveryTracker(int total, Completion completion) {
        this.total = total;
        this.remaining = total;
        this.completion = completion;
    }

    synchronized void record(String userId, boolean succeeded) {
        if (remaining <= 0) return;
        if (!succeeded) failed.add(userId);
        remaining -= 1;
        if (remaining == 0 && completion != null) {
            completion.onFinished(total, Collections.unmodifiableList(new ArrayList<>(failed)));
        }
    }
}
