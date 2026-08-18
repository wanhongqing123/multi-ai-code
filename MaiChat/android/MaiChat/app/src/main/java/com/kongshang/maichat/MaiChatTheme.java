package com.kongshang.maichat;

import android.content.Context;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.view.Gravity;
import android.widget.TextView;

public final class MaiChatTheme {
    public static final int PAGE = Color.rgb(246, 249, 252);
    public static final int PANEL = Color.WHITE;
    public static final int BORDER = Color.rgb(218, 228, 240);
    public static final int TEXT = Color.rgb(14, 21, 37);
    public static final int SECONDARY = Color.rgb(100, 117, 143);
    public static final int BLUE = Color.rgb(15, 141, 221);
    public static final int BLUE_DARK = Color.rgb(9, 96, 170);
    public static final int BLUE_SOFT = Color.rgb(225, 244, 255);
    public static final int GREEN = Color.rgb(16, 152, 83);
    public static final int GREEN_SOFT = Color.rgb(216, 251, 230);
    public static final int YELLOW_SOFT = Color.rgb(255, 251, 233);
    public static final int YELLOW_BORDER = Color.rgb(253, 207, 88);
    public static final int RED = Color.rgb(220, 38, 38);

    private MaiChatTheme() {
    }

    public static int dp(Context context, float value) {
        return Math.round(value * context.getResources().getDisplayMetrics().density);
    }

    public static GradientDrawable rounded(int color, float radiusDp, Context context) {
        GradientDrawable drawable = new GradientDrawable();
        drawable.setColor(color);
        drawable.setCornerRadius(dp(context, radiusDp));
        return drawable;
    }

    public static GradientDrawable bordered(
        int fill,
        int stroke,
        float radiusDp,
        Context context
    ) {
        GradientDrawable drawable = rounded(fill, radiusDp, context);
        drawable.setStroke(dp(context, 1), stroke);
        return drawable;
    }

    public static GradientDrawable gradientAvatar(boolean outgoing, Context context) {
        int[] colors = outgoing
            ? new int[]{Color.rgb(91, 155, 255), Color.rgb(30, 64, 175)}
            : new int[]{Color.rgb(45, 212, 191), Color.rgb(15, 118, 110)};
        GradientDrawable drawable = new GradientDrawable(
            GradientDrawable.Orientation.TL_BR,
            colors
        );
        drawable.setCornerRadius(dp(context, 10));
        return drawable;
    }

    public static TextView text(Context context, String value, float sizeSp, int color) {
        TextView view = new TextView(context);
        view.setText(value);
        view.setTextSize(sizeSp);
        view.setTextColor(color);
        view.setGravity(Gravity.CENTER_VERTICAL);
        view.setIncludeFontPadding(false);
        return view;
    }

    public static TextView label(Context context, String value, float sizeSp, int color) {
        TextView view = text(context, value, sizeSp, color);
        view.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        return view;
    }

}
