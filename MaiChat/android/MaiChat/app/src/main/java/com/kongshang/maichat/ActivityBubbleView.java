package com.kongshang.maichat;

import android.animation.ValueAnimator;
import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.RectF;
import android.view.View;

/** Only this small row animates; message history and composer stay untouched. */
final class ActivityBubbleView extends View {
    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final ValueAnimator animation = ValueAnimator.ofFloat(0, 1);
    private final RemoteIMActivitySignal.Kind kind;
    private final float scale;
    private float phase;
    ActivityBubbleView(Context context, RemoteIMActivitySignal.Kind kind) {
        super(context);
        this.kind = kind;
        scale = getResources().getDisplayMetrics().density;
        setContentDescription(kind == RemoteIMActivitySignal.Kind.HUMAN_TYPING ? "对方正在输入" : kind.label);
        animation.setDuration(1200);
        animation.setRepeatCount(ValueAnimator.INFINITE);
        animation.addUpdateListener(value -> { phase = (float) value.getAnimatedValue(); invalidate(); });
    }
    @Override protected void onMeasure(int width, int height) {
        setMeasuredDimension(MeasureSpec.getSize(width), Math.round(48 * scale));
    }
    @Override protected void onAttachedToWindow() { super.onAttachedToWindow(); updateAnimation(); }
    @Override protected void onDetachedFromWindow() { animation.cancel(); super.onDetachedFromWindow(); }
    @Override protected void onWindowVisibilityChanged(int visibility) { super.onWindowVisibilityChanged(visibility); updateAnimation(); }
    private void updateAnimation() {
        if (animation == null) return;
        if (isAttachedToWindow() && getWindowVisibility() == VISIBLE && ValueAnimator.areAnimatorsEnabled()) {
            if (!animation.isStarted()) animation.start();
        } else animation.cancel();
    }
    @Override protected void onDraw(Canvas canvas) {
        paint.setStyle(Paint.Style.FILL);
        paint.setTextSize(12 * getResources().getDisplayMetrics().scaledDensity);
        float width = kind == RemoteIMActivitySignal.Kind.HUMAN_TYPING ? 60 * scale : 48 * scale + paint.measureText(kind.label);
        paint.setColor(android.graphics.Color.rgb(242, 242, 247));
        canvas.drawRoundRect(new RectF(0, 3 * scale, width, 45 * scale), 21 * scale, 21 * scale, paint);
        paint.setColor(MaiChatTheme.BLUE);
        if (kind == RemoteIMActivitySignal.Kind.HUMAN_TYPING) {
            for (int i = 0; i < 3; i++) {
                double wave = (1 + Math.sin((phase - i * .16) * 2 * Math.PI)) / 2;
                paint.setAlpha((int) (80 + 175 * wave));
                canvas.drawCircle((18 + i * 12) * scale, (24 - (float) wave * 2) * scale, 3.5f * scale, paint);
            }
            paint.setAlpha(255);
        } else {
            paint.setStyle(Paint.Style.STROKE); paint.setStrokeWidth(1.8f * scale);
            canvas.drawArc(new RectF(13 * scale, 16 * scale, 27 * scale, 30 * scale), phase * 360, 260, false, paint);
            paint.setStyle(Paint.Style.FILL);
            paint.setColor(MaiChatTheme.SECONDARY);
            canvas.drawText(kind.label, 36 * scale, 23 * scale - (paint.ascent() + paint.descent()) / 2, paint);
        }
    }
}
