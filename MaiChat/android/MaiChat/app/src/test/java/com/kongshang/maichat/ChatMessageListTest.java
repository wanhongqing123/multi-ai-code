package com.kongshang.maichat;

import android.view.View;
import android.widget.TextView;
import androidx.test.core.app.ApplicationProvider;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;
import java.util.ArrayList;
import java.util.List;
import static org.junit.Assert.*;

@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34)
public class ChatMessageListTest {
    private final List<RemoteIMMessage> messages = new ArrayList<>();
    private int constructed;
    private ChatMessageList list() {
        return new ChatMessageList(ApplicationProvider.getApplicationContext(), message -> {
            constructed++;
            TextView text = new TextView(ApplicationProvider.getApplicationContext());
            text.setText(message.text()); text.setHeight(100);
            return text;
        });
    }
    private void add(int count) {
        for (int n = 0; n < count; n++) {
            int i = messages.size();
            messages.add(new RemoteIMMessage("id" + i, "remote" + i, "peer", "owner", "message " + i,
                RemoteIMMessage.Direction.INCOMING, RemoteIMMessage.Status.RECEIVED, i, null, null, null, null, RemoteIMOrigin.HUMAN));
        }
    }
    private void layout(ChatMessageList list) {
        list.measure(View.MeasureSpec.makeMeasureSpec(400, View.MeasureSpec.EXACTLY), View.MeasureSpec.makeMeasureSpec(600, View.MeasureSpec.EXACTLY));
        list.layout(0, 0, 400, 600);
    }
    @Test public void enteringAndSendingScrollButIncomingAndReceiptsPreserveTheAnchor() {
        add(40); ChatMessageList list = list();
        list.requestLatestAfterUpdate(); list.update(messages, null, 0); layout(list);
        assertEquals(39, list.getLastVisiblePosition());
        assertTrue("Only visible rows should be mounted", constructed < 20);
        list.showMessage("id10"); layout(list);
        String anchor = list.firstVisibleMessageId(); int offset = list.firstVisibleOffset();
        add(1); list.update(messages, null, 0); layout(list);
        assertEquals(anchor, list.firstVisibleMessageId()); assertEquals(offset, list.firstVisibleOffset());
        messages.get(40).setStatus(RemoteIMMessage.Status.SENT);
        list.update(messages, null, 0); layout(list);
        assertEquals(anchor, list.firstVisibleMessageId());
        list.requestLatestAfterUpdate(); add(1); list.update(messages, null, 0); layout(list);
        assertEquals(41, list.getLastVisiblePosition());
        list.showMessage("id12"); layout(list); list.showLatest(); layout(list);
        assertEquals(41, list.getLastVisiblePosition());
    }
    @Test public void aLongNewestMessageAlignsItsBottomAndALaterReplyDoesNotReplaceTheOwnSendTarget() {
        add(5);
        ChatMessageList list = new ChatMessageList(ApplicationProvider.getApplicationContext(), message -> {
            TextView view = new TextView(ApplicationProvider.getApplicationContext());
            view.setText(message.text()); view.setHeight(1400); return view;
        });
        list.requestLatestAfterUpdate(); list.update(messages, null, 0); layout(list);
        View last = list.getChildAt(list.getCount() - 1 - list.getFirstVisiblePosition());
        assertEquals(list.getHeight() - list.getPaddingBottom(), last.getBottom());
        add(1); list.requestMessageBottomAfterUpdate("id5"); add(1);
        list.update(messages, null, 0); layout(list);
        View own = list.getChildAt(5 - list.getFirstVisiblePosition());
        assertEquals(list.getHeight() - list.getPaddingBottom(), own.getBottom());
    }

    @Test public void activityIsVisibleAtLatestWithoutMovingTheMessageAnchor() {
        add(40); ChatMessageList list = list();
        list.requestLatestAfterUpdate(); list.update(messages, null, 0); layout(list);
        String anchor = list.firstVisibleMessageId(); int offset = list.firstVisibleOffset();
        RemoteIMActivitySignal activity = new RemoteIMActivitySignal("visible", 1, RemoteIMActivitySignal.Kind.HUMAN_TYPING, true, 12000);
        list.update(messages, activity, 0); layout(list);
        assertEquals(anchor, list.firstVisibleMessageId()); assertEquals(offset, list.firstVisibleOffset());
        assertEquals(40, list.getLastVisiblePosition());
    }

    @Test public void prependedHistoryAndTransientActivityDoNotPullTheReaderDown() {
        add(30); ChatMessageList list = list();
        list.update(new ArrayList<>(messages.subList(10, 30)), null, 0); layout(list);
        list.showMessage("id15"); layout(list);
        String anchor = list.firstVisibleMessageId(); int offset = list.firstVisibleOffset();
        list.update(messages, null, 0); layout(list);
        assertEquals(anchor, list.firstVisibleMessageId()); assertEquals(offset, list.firstVisibleOffset());
        RemoteIMActivitySignal activity = new RemoteIMActivitySignal("a", 1, RemoteIMActivitySignal.Kind.MACHINE_TOOL, true, 12000);
        list.update(messages, activity, 0); layout(list);
        assertEquals(anchor, list.firstVisibleMessageId()); assertEquals(31, list.getCount());
        add(1); list.update(messages, null, 0); layout(list);
        assertEquals(anchor, list.firstVisibleMessageId()); assertEquals(31, list.getCount());
    }
}
