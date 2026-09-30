package com.kongshang.maichat;

import android.animation.Animator;
import android.animation.AnimatorListenerAdapter;
import android.animation.ValueAnimator;
import android.os.Build;
import android.view.MotionEvent;
import android.view.VelocityTracker;
import android.view.View;
import android.view.animation.DecelerateInterpolator;
import java.util.function.Function;

/** Full-page horizontal navigation; vertical history scrolling keeps priority. */
final class ChatSwipeBack {
    interface Host {
        View page(); boolean canGoBack(); boolean keyboardVisible();
        void began(); void hideKeyboard(); void completed(); void cancelled(boolean keyboardShown);
    }
    private final Host host;
    private float startX, startY, progress;
    private boolean rejected, dragging, animating;
    private VelocityTracker velocity;
    private Keyboard keyboard;
    private ValueAnimator runningAnimator;
    private boolean closed;
    ChatSwipeBack(Host host) { this.host = host; }
    boolean active() { return dragging || animating; }
    void dispose() {
        closed = true;
        if (runningAnimator != null) runningAnimator.cancel();
        if (keyboard != null && active()) keyboard.finish(false, () -> { });
        if (velocity != null) { velocity.recycle(); velocity = null; }
    }
    boolean dispatch(MotionEvent event, Function<MotionEvent, Boolean> fallback) {
        if (closed || !host.canGoBack()) return fallback.apply(event);
        if (animating) return true;
        View page = host.page();
        float threshold = 16 * page.getResources().getDisplayMetrics().density;
        switch (event.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
                startX = event.getX(); startY = event.getY(); rejected = false;
                if (velocity != null) velocity.recycle();
                velocity = VelocityTracker.obtain(); velocity.addMovement(event);
                return fallback.apply(event);
            case MotionEvent.ACTION_MOVE:
                if (velocity != null) velocity.addMovement(event);
                float dx = event.getX() - startX, dy = event.getY() - startY;
                if (!dragging && !rejected) {
                    if (dx > threshold && dx > Math.abs(dy) * 1.4f) {
                        MotionEvent cancel = MotionEvent.obtain(event); cancel.setAction(MotionEvent.ACTION_CANCEL);
                        fallback.apply(cancel); cancel.recycle();
                        begin();
                    } else if (Math.abs(dy) > threshold || dx < -threshold) rejected = true;
                }
                if (dragging) { update(Math.max(0, Math.min(1, dx / Math.max(1, page.getWidth())))); return true; }
                return fallback.apply(event);
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_CANCEL:
                if (velocity != null) { velocity.addMovement(event); velocity.computeCurrentVelocity(1000); }
                boolean complete = event.getActionMasked() == MotionEvent.ACTION_UP &&
                    (progress > .3f || velocity != null && velocity.getXVelocity() > 900 * page.getResources().getDisplayMetrics().density);
                if (velocity != null) { velocity.recycle(); velocity = null; }
                if (dragging) { settle(complete); return true; }
                return fallback.apply(event);
            default: return fallback.apply(event);
        }
    }
    void goBack() { if (host.canGoBack() && !active()) { begin(); settle(true); } }
    private void begin() {
        dragging = true; host.began();
        keyboard = new Keyboard() {
            @Override public void progress(float value) { }
            @Override public void finish(boolean shown, Runnable complete) { complete.run(); }
            @Override public boolean controlled() { return false; }
        };
        if (host.keyboardVisible() && Build.VERSION.SDK_INT >= 30 && host.page().getWindowInsetsController() != null) {
            keyboard = new Api30Keyboard(host.page(), host::hideKeyboard);
        } else host.hideKeyboard();
    }
    private void update(float value) {
        if (closed) return;
        progress = value; host.page().setTranslationX(value * host.page().getWidth());
        if (keyboard != null) keyboard.progress(value);
    }
    private void settle(boolean complete) {
        dragging = false; animating = true;
        ValueAnimator animator = ValueAnimator.ofFloat(progress, complete ? 1 : 0);
        runningAnimator = animator;
        animator.setInterpolator(new DecelerateInterpolator()); animator.setDuration(220);
        animator.addUpdateListener(value -> update((float) value.getAnimatedValue()));
        animator.addListener(new AnimatorListenerAdapter() {
            @Override public void onAnimationEnd(Animator animation) {
                if (closed) return;
                Keyboard activeKeyboard = keyboard;
                activeKeyboard.finish(!complete, () -> {
                    if (closed) return;
                    animating = false; progress = 0;
                    if (complete) { host.hideKeyboard(); host.completed(); }
                    else {
                        host.page().setTranslationX(0);
                        androidx.core.view.WindowInsetsCompat insets = androidx.core.view.ViewCompat.getRootWindowInsets(host.page());
                        host.cancelled(activeKeyboard.controlled() || insets != null && insets.isVisible(androidx.core.view.WindowInsetsCompat.Type.ime()));
                    }
                    keyboard = null;
                });
            }
        });
        animator.start();
    }
    private interface Keyboard { void progress(float value); void finish(boolean shown, Runnable complete); boolean controlled(); }

    @androidx.annotation.RequiresApi(30)
    private static final class Api30Keyboard implements Keyboard, android.view.WindowInsetsAnimationControlListener {
        private android.view.WindowInsetsAnimationController controller;
        private android.graphics.Insets start, hidden;
        private float progress;
        private Boolean finishShown;
        private Runnable completion;
        private final Runnable fallback;
        private boolean unavailable;
        private final android.os.CancellationSignal cancellation = new android.os.CancellationSignal();
        Api30Keyboard(View page, Runnable fallback) {
            this.fallback = fallback;
            page.getWindowInsetsController().controlWindowInsetsAnimation(android.view.WindowInsets.Type.ime(), -1, null,
                cancellation, this);
        }
        @Override public void onReady(android.view.WindowInsetsAnimationController controller, int types) {
            this.controller = controller; start = controller.getCurrentInsets(); hidden = controller.getHiddenStateInsets();
            progress(progress);
            if (finishShown != null) controller.finish(finishShown);
        }
        @Override public void progress(float value) {
            progress = value;
            if (controller != null && !controller.isFinished() && !controller.isCancelled()) {
                controller.setInsetsAndAlpha(android.graphics.Insets.of(start.left, start.top, start.right,
                    Math.round(start.bottom + (hidden.bottom - start.bottom) * value)), 1, value);
            }
        }
        @Override public void finish(boolean shown, Runnable complete) {
            finishShown = shown; completion = complete;
            if (unavailable) finishCallback();
            else if (controller != null) {
                if (controller.isFinished() || controller.isCancelled()) finishCallback();
                else controller.finish(shown);
            }
            else {
                unavailable = true;
                cancellation.cancel();
                fallback.run(); finishCallback();
            }
        }
        @Override public void onFinished(android.view.WindowInsetsAnimationController controller) { finishCallback(); }
        @Override public void onCancelled(android.view.WindowInsetsAnimationController controller) {
            unavailable = true; fallback.run(); finishCallback();
        }
        @Override public boolean controlled() { return !unavailable; }
        private void finishCallback() {
            if (completion != null) { Runnable next = completion; completion = null; next.run(); }
        }
    }
}
