package com.kongshang.maichat;

import android.annotation.SuppressLint;
import android.content.Context;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewConfiguration;
import android.widget.FrameLayout;
import android.widget.TextView;

import java.lang.ref.WeakReference;

@SuppressLint("ViewConstructor")
public final class SwipeActionRow extends FrameLayout {
    private static WeakReference<SwipeActionRow> openRow = new WeakReference<>(null);
    private final View content;
    private final TextView action;
    private final int actionWidth;
    private float downX;
    private float downY;
    private boolean dragging;
    private final int horizontalThreshold;

    public SwipeActionRow(
        Context context,
        View content,
        String actionTitle,
        int actionColor,
        Runnable actionHandler
    ) {
        super(context);
        this.content = content;
        actionWidth = MaiChatTheme.dp(context, 92);
        horizontalThreshold = Math.max(
            ViewConfiguration.get(context).getScaledTouchSlop() * 3,
            MaiChatTheme.dp(context, 24)
        );
        action = MaiChatTheme.label(context, actionTitle, 14, android.graphics.Color.WHITE);
        action.setGravity(android.view.Gravity.CENTER);
        action.setBackgroundColor(actionColor);
        action.setOnClickListener(view -> {
            close();
            actionHandler.run();
        });
        addView(action, new LayoutParams(actionWidth, LayoutParams.MATCH_PARENT, android.view.Gravity.END));
        addView(content, new LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.MATCH_PARENT));
        setClipChildren(true);
        setBackgroundColor(MaiChatTheme.PANEL);
    }

    public void close() {
        content.animate().translationX(0).setDuration(150).start();
        if (openRow.get() == this) openRow = new WeakReference<>(null);
    }

    @Override
    public boolean onInterceptTouchEvent(MotionEvent event) {
        switch (event.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
                downX = event.getX();
                downY = event.getY();
                dragging = false;
                break;
            case MotionEvent.ACTION_MOVE:
                float dx = event.getX() - downX;
                float dy = event.getY() - downY;
                if (Math.abs(dy) > horizontalThreshold && Math.abs(dy) >= Math.abs(dx)) {
                    close();
                    return false;
                }
                if (dx < -horizontalThreshold && Math.abs(dx) > Math.abs(dy) * 1.6f) {
                    dragging = true;
                    getParent().requestDisallowInterceptTouchEvent(true);
                    return true;
                }
                break;
            default:
                break;
        }
        return false;
    }

    @Override
    public boolean onTouchEvent(MotionEvent event) {
        switch (event.getActionMasked()) {
            case MotionEvent.ACTION_MOVE:
                float translation = Math.max(-actionWidth, Math.min(0, event.getX() - downX));
                content.setTranslationX(translation);
                return true;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_CANCEL:
                float dx = event.getX() - downX;
                float dy = event.getY() - downY;
                boolean reveal = event.getActionMasked() == MotionEvent.ACTION_UP
                    && dx < -Math.max(actionWidth * 0.58f, horizontalThreshold)
                    && Math.abs(dx) > Math.abs(dy) * 1.6f;
                if (reveal) {
                    SwipeActionRow previous = openRow.get();
                    if (previous != null && previous != this) previous.close();
                    openRow = new WeakReference<>(this);
                }
                content.animate()
                    .translationX(reveal ? -actionWidth : 0)
                    .setDuration(150)
                    .start();
                getParent().requestDisallowInterceptTouchEvent(false);
                dragging = false;
                if (event.getActionMasked() == MotionEvent.ACTION_UP) performClick();
                return true;
            default:
                return dragging || super.onTouchEvent(event);
        }
    }

    @Override
    public boolean performClick() {
        super.performClick();
        return true;
    }
}
