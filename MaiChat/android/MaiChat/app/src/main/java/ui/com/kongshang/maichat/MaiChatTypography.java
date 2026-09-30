package com.kongshang.maichat;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Typeface;
import android.os.Build;
import android.text.Layout;
import android.text.Spanned;
import android.text.TextPaint;
import android.text.style.LeadingMarginSpan;
import android.text.style.MetricAffectingSpan;
import android.widget.TextView;

/** Matches the effective iOS MarkdownLikeText metrics using Android system fonts. */
final class MaiChatTypography {
    static final int BODY_SP = 14;
    static final int DATE_SP = 11;
    static final int INLINE_CODE_SP = 13;
    static final int CODE_BLOCK_SP = 12;
    static final int CODE_COLOR = android.graphics.Color.rgb(107, 59, 150);
    static final int CODE_BACKGROUND = android.graphics.Color.rgb(242, 237, 250);
    static final int HEADING_COLOR = android.graphics.Color.rgb(28, 79, 138);
    static Typeface semibold() {
        return Build.VERSION.SDK_INT >= 28 ? Typeface.create(Typeface.SANS_SERIF, 600, false)
            : Typeface.create("sans-serif-medium", Typeface.NORMAL);
    }
    static TextView body(Context context) {
        TextView view = MaiChatTheme.text(context, "", BODY_SP, MaiChatTheme.TEXT);
        view.setTypeface(Typeface.SANS_SERIF, Typeface.NORMAL);
        view.setLineSpacing(MaiChatTheme.dp(context, 4), 1);
        view.setTextIsSelectable(true);
        return view;
    }
    static final class SemiboldSpan extends MetricAffectingSpan {
        @Override public void updateDrawState(TextPaint paint) { paint.setTypeface(semibold()); }
        @Override public void updateMeasureState(TextPaint paint) { paint.setTypeface(semibold()); }
    }
    static final class BlueBulletSpan implements LeadingMarginSpan {
        private final int margin;
        private final float radius, gap;
        BlueBulletSpan(float density, int level) { margin = Math.round((level == 0 ? 24 : level > 4 ? 0 : 12) * density); radius = 1.5f * density; gap = 9 * density; }
        @Override public int getLeadingMargin(boolean first) { return margin; }
        @Override public void drawLeadingMargin(Canvas canvas, Paint paint, int x, int direction, int top, int baseline, int bottom,
            CharSequence text, int start, int end, boolean first, Layout layout) {
            if (!first || !(text instanceof Spanned) || ((Spanned) text).getSpanStart(this) != start) return;
            int color = paint.getColor(); Paint.Style style = paint.getStyle();
            paint.setColor(MaiChatTheme.BLUE); paint.setStyle(Paint.Style.FILL);
            canvas.drawCircle(x + direction * (margin - gap), baseline + (paint.ascent() + paint.descent()) / 2, radius, paint);
            paint.setColor(color); paint.setStyle(style);
        }
    }
}
