package com.kongshang.maichat;

import android.content.Context;
import android.view.Gravity;
import android.view.View;
import android.widget.FrameLayout;
import android.widget.TextView;

/** Displays media images only through the shared Graphics presenter. */
public final class MaiGraphicsImageView extends FrameLayout {
    private final MaiGraphicsTextureView surface;
    private final TextView failureLabel;
    private Runnable missingHandler;
    private MaiGraphicsTextureView.PresentationListener presentationListener;

    public MaiGraphicsImageView(Context context) {
        super(context);
        surface = new MaiGraphicsTextureView(context);
        surface.setAlpha(0.01f);
        surface.setClickable(false);
        surface.setPresentationListener(this::onPresented);
        addView(surface, new LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.MATCH_PARENT));

        failureLabel = new TextView(context);
        failureLabel.setText("图片无法显示");
        failureLabel.setTextColor(MaiChatTheme.SECONDARY);
        failureLabel.setGravity(Gravity.CENTER);
        failureLabel.setBackgroundColor(MaiChatTheme.PAGE);
        failureLabel.setVisibility(View.GONE);
        addView(failureLabel,
            new LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.MATCH_PARENT));
    }

    public void showImage(String path, boolean fill, Runnable missing) {
        missingHandler = missing;
        failureLabel.setVisibility(View.GONE);
        surface.setAlpha(0.01f);
        surface.setImageFile(path, fill);
    }

    public void setPresentationListener(MaiGraphicsTextureView.PresentationListener listener) {
        presentationListener = listener;
    }

    private void onPresented(boolean success) {
        if (presentationListener != null) presentationListener.onPresented(success);
        if (success) {
            failureLabel.setVisibility(View.GONE);
            surface.setAlpha(1.0f);
            return;
        }
        surface.setAlpha(0.01f);
        failureLabel.setVisibility(View.VISIBLE);
        if (missingHandler != null) missingHandler.run();
    }
}
