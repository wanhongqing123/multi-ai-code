package com.kongshang.maichat;

import android.annotation.SuppressLint;
import android.content.Context;
import android.view.MotionEvent;
import android.view.View;
import android.widget.FrameLayout;
import android.widget.TextView;

@SuppressLint("ViewConstructor")
public final class SwipeActionRow extends FrameLayout {
    private final View content;
    private final TextView action;
    private final int actionWidth;
    private float downX;
    private float downY;
    private boolean dragging;

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
        action = MaiChatTheme.label(context, actionTitle, 14, android.graphics.Color.WHITE);
        action.setGravity(android.view.Gravity.CENTER);
        action.setBackgroundColor(actionColor);
        action.setOnClickListener(view -> {
            close();
            actionHandler.run();
        });
        addView(action, new LayoutParams(actionWidth, LayoutParams.MATCH_PARENT, android.view.Gravity.END));
        addView(content, new LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.WRAP_CONTENT));
        setClipChildren(true);
        setBackgroundColor(MaiChatTheme.PANEL);
    }

    public void close() {
        content.animate().translationX(0).setDuration(150).start();
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
                if (Math.abs(dx) > MaiChatTheme.dp(getContext(), 8) && Math.abs(dx) > Math.abs(dy)) {
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
                boolean reveal = content.getTranslationX() < -actionWidth * 0.42f;
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
