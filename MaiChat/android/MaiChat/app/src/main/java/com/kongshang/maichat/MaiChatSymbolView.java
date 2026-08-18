package com.kongshang.maichat;

import android.annotation.SuppressLint;
import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import android.view.View;

@SuppressLint({"ViewConstructor", "DrawAllocation"})
public final class MaiChatSymbolView extends View {
    public enum Symbol {
        MESSAGE,
        CONTACTS,
        REMOTE,
        USER,
        SPEAKER,
        KEYBOARD,
        CHEVRON_LEFT,
        PLUS
    }

    private final Symbol symbol;
    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);

    public MaiChatSymbolView(Context context, Symbol symbol) {
        super(context);
        this.symbol = symbol;
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(MaiChatTheme.dp(context, 1.6f));
        paint.setStrokeCap(Paint.Cap.ROUND);
        paint.setStrokeJoin(Paint.Join.ROUND);
    }

    public void setSymbolColor(int color) {
        paint.setColor(color);
        invalidate();
    }

    @Override
    protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        float width = getWidth();
        float height = getHeight();
        float size = Math.min(width, height) * 0.64f;
        float left = (width - size) / 2f;
        float top = (height - size) / 2f;
        RectF bounds = new RectF(left, top, left + size, top + size);
        switch (symbol) {
            case MESSAGE:
                drawMessage(canvas, bounds);
                break;
            case CONTACTS:
                drawContacts(canvas, bounds);
                break;
            case REMOTE:
                drawRemote(canvas, bounds);
                break;
            case USER:
                drawUser(canvas, bounds);
                break;
            case SPEAKER:
                drawSpeaker(canvas, bounds);
                break;
            case KEYBOARD:
                drawKeyboard(canvas, bounds);
                break;
            case CHEVRON_LEFT:
                drawChevronLeft(canvas, bounds);
                break;
            case PLUS:
                drawPlus(canvas, bounds);
                break;
        }
    }

    private void drawMessage(Canvas canvas, RectF bounds) {
        RectF bubble = new RectF(
            bounds.left,
            bounds.top,
            bounds.right,
            bounds.bottom - bounds.height() * 0.16f
        );
        canvas.drawRoundRect(bubble, bounds.width() * 0.18f, bounds.width() * 0.18f, paint);
        Path tail = new Path();
        tail.moveTo(bounds.left + bounds.width() * 0.26f, bubble.bottom);
        tail.lineTo(bounds.left + bounds.width() * 0.18f, bounds.bottom);
        tail.lineTo(bounds.left + bounds.width() * 0.46f, bubble.bottom);
        canvas.drawPath(tail, paint);
    }

    private void drawContacts(Canvas canvas, RectF bounds) {
        float radius = bounds.width() * 0.16f;
        canvas.drawCircle(bounds.left + bounds.width() * 0.36f, bounds.top + radius, radius, paint);
        canvas.drawCircle(bounds.left + bounds.width() * 0.72f, bounds.top + radius * 1.35f, radius * 0.82f, paint);
        canvas.drawArc(
            new RectF(bounds.left, bounds.top + bounds.height() * 0.35f, bounds.left + bounds.width() * 0.72f, bounds.bottom),
            195,
            150,
            false,
            paint
        );
        canvas.drawArc(
            new RectF(bounds.left + bounds.width() * 0.42f, bounds.top + bounds.height() * 0.45f, bounds.right, bounds.bottom),
            200,
            135,
            false,
            paint
        );
    }

    private void drawRemote(Canvas canvas, RectF bounds) {
        RectF monitor = new RectF(bounds.left, bounds.top, bounds.right, bounds.bottom * 0.88f + bounds.top * 0.12f);
        canvas.drawRoundRect(monitor, bounds.width() * 0.08f, bounds.width() * 0.08f, paint);
        float centerX = bounds.centerX();
        float standTop = monitor.bottom;
        canvas.drawLine(centerX, standTop, centerX, bounds.bottom, paint);
        canvas.drawLine(centerX - bounds.width() * 0.2f, bounds.bottom, centerX + bounds.width() * 0.2f, bounds.bottom, paint);
    }

    private void drawUser(Canvas canvas, RectF bounds) {
        float radius = bounds.width() * 0.2f;
        canvas.drawCircle(bounds.centerX(), bounds.top + radius, radius, paint);
        canvas.drawArc(
            new RectF(bounds.left, bounds.top + bounds.height() * 0.38f, bounds.right, bounds.bottom),
            195,
            150,
            false,
            paint
        );
    }

    private void drawSpeaker(Canvas canvas, RectF bounds) {
        Path speaker = new Path();
        speaker.moveTo(bounds.left, bounds.centerY() - bounds.height() * 0.14f);
        speaker.lineTo(bounds.left + bounds.width() * 0.24f, bounds.centerY() - bounds.height() * 0.14f);
        speaker.lineTo(bounds.left + bounds.width() * 0.48f, bounds.top + bounds.height() * 0.18f);
        speaker.lineTo(bounds.left + bounds.width() * 0.48f, bounds.bottom - bounds.height() * 0.18f);
        speaker.lineTo(bounds.left + bounds.width() * 0.24f, bounds.centerY() + bounds.height() * 0.14f);
        speaker.lineTo(bounds.left, bounds.centerY() + bounds.height() * 0.14f);
        speaker.close();
        canvas.drawPath(speaker, paint);
        canvas.drawArc(
            new RectF(
                bounds.left + bounds.width() * 0.38f,
                bounds.top + bounds.height() * 0.24f,
                bounds.left + bounds.width() * 0.76f,
                bounds.bottom - bounds.height() * 0.24f
            ),
            -58,
            116,
            false,
            paint
        );
        canvas.drawArc(
            new RectF(
                bounds.left + bounds.width() * 0.30f,
                bounds.top + bounds.height() * 0.06f,
                bounds.right,
                bounds.bottom - bounds.height() * 0.06f
            ),
            -52,
            104,
            false,
            paint
        );
    }

    private void drawKeyboard(Canvas canvas, RectF bounds) {
        canvas.drawRoundRect(
            bounds,
            bounds.width() * 0.10f,
            bounds.width() * 0.10f,
            paint
        );
        float top = bounds.top + bounds.height() * 0.30f;
        float rowSpacing = bounds.height() * 0.22f;
        for (int row = 0; row < 2; row += 1) {
            float y = top + row * rowSpacing;
            for (int column = 0; column < 4; column += 1) {
                float x = bounds.left + bounds.width() * (0.18f + column * 0.21f);
                canvas.drawPoint(x, y, paint);
            }
        }
        canvas.drawLine(
            bounds.left + bounds.width() * 0.24f,
            bounds.bottom - bounds.height() * 0.18f,
            bounds.right - bounds.width() * 0.24f,
            bounds.bottom - bounds.height() * 0.18f,
            paint
        );
    }

    private void drawChevronLeft(Canvas canvas, RectF bounds) {
        Path path = new Path();
        path.moveTo(bounds.right - bounds.width() * 0.28f, bounds.top + bounds.height() * 0.12f);
        path.lineTo(bounds.left + bounds.width() * 0.30f, bounds.centerY());
        path.lineTo(bounds.right - bounds.width() * 0.28f, bounds.bottom - bounds.height() * 0.12f);
        canvas.drawPath(path, paint);
    }

    private void drawPlus(Canvas canvas, RectF bounds) {
        canvas.drawLine(bounds.centerX(), bounds.top, bounds.centerX(), bounds.bottom, paint);
        canvas.drawLine(bounds.left, bounds.centerY(), bounds.right, bounds.centerY(), paint);
    }
}
