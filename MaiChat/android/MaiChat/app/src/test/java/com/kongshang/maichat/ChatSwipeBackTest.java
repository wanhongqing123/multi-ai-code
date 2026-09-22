package com.kongshang.maichat;

import android.os.Looper;
import android.view.MotionEvent;
import android.view.View;
import android.widget.FrameLayout;
import androidx.test.core.app.ApplicationProvider;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.Shadows;
import org.robolectric.annotation.Config;
import java.time.Duration;
import static org.junit.Assert.*;

@RunWith(RobolectricTestRunner.class)
@Config(sdk = 29)
public class ChatSwipeBackTest {
    private static final class Host implements ChatSwipeBack.Host {
        final View page = new FrameLayout(ApplicationProvider.getApplicationContext());
        int began, completed, cancelled, hidden;
        Host() { page.layout(0, 0, 600, 900); }
        public View page() { return page; }
        public boolean canGoBack() { return true; }
        public boolean keyboardVisible() { return true; }
        public void began() { began++; }
        public void hideKeyboard() { hidden++; }
        public void completed() { completed++; }
        public void cancelled(boolean shown) { cancelled++; }
    }
    private void touch(ChatSwipeBack back, int action, long time, float x, float y) {
        MotionEvent event = MotionEvent.obtain(0, time, action, x, y, 0);
        back.dispatch(event, value -> true); event.recycle();
    }
    @Test public void swipeCanBeginAwayFromTheEdgeAndCancelWithoutEnteringAgain() {
        Host host = new Host(); ChatSwipeBack back = new ChatSwipeBack(host);
        touch(back, MotionEvent.ACTION_DOWN, 0, 250, 300);
        touch(back, MotionEvent.ACTION_MOVE, 300, 400, 320);
        assertEquals(1, host.began); assertEquals(1, host.hidden);
        assertEquals(150f, host.page.getTranslationX(), .01f);
        touch(back, MotionEvent.ACTION_CANCEL, 600, 400, 320);
        Shadows.shadowOf(Looper.getMainLooper()).idleFor(Duration.ofMillis(600));
        assertEquals(0, host.completed); assertEquals(1, host.cancelled);
        assertEquals(0f, host.page.getTranslationX(), .01f);
    }
    @Test public void verticalHistoryScrollDoesNotBeginBackNavigation() {
        Host host = new Host(); ChatSwipeBack back = new ChatSwipeBack(host);
        touch(back, MotionEvent.ACTION_DOWN, 0, 250, 300);
        touch(back, MotionEvent.ACTION_MOVE, 100, 265, 410);
        touch(back, MotionEvent.ACTION_UP, 200, 280, 500);
        assertEquals(0, host.began); assertEquals(0, host.hidden);
    }
    @Test public void finishingTheSwipePopsOnce() {
        Host host = new Host(); ChatSwipeBack back = new ChatSwipeBack(host);
        touch(back, MotionEvent.ACTION_DOWN, 0, 180, 300);
        touch(back, MotionEvent.ACTION_MOVE, 400, 560, 310);
        touch(back, MotionEvent.ACTION_UP, 700, 560, 310);
        Shadows.shadowOf(Looper.getMainLooper()).idleFor(Duration.ofMillis(600));
        assertEquals(1, host.completed); assertEquals(0, host.cancelled);
    }
}
