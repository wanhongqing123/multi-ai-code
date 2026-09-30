package com.kongshang.maichat;

import android.content.Context;
import android.view.View;
import android.widget.FrameLayout;
import android.widget.ImageView;

/** Displays images directly through Graphics, with a decoder fallback on failure. */
public final class MaiGraphicsImageView extends FrameLayout {
    private final MaiGraphicsTextureView surface;
    private ImageView fallback;
    private String imagePath;
    private int requestedWidth;
    private int requestedHeight;
    private boolean fillView;
    private MessageImageLoader.MissingHandler missingHandler;
    private MaiGraphicsTextureView.PresentationListener presentationListener;

    public MaiGraphicsImageView(Context context) {
        super(context);
        surface = new MaiGraphicsTextureView(context);
        surface.setAlpha(0.01f);
        surface.setClickable(false);
        surface.setPresentationListener(this::onPresented);
        addView(surface, new LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.MATCH_PARENT));
    }

    public void showImage(String path, int width, int height, boolean fill,
                          MessageImageLoader.MissingHandler missing) {
        imagePath = path;
        requestedWidth = width;
        requestedHeight = height;
        fillView = fill;
        missingHandler = missing;
        surface.setAlpha(0.01f);
        surface.setImageFile(path, fill);
        if (fallback != null) fallback.setVisibility(View.GONE);
    }

    public void setPresentationListener(MaiGraphicsTextureView.PresentationListener listener) {
        presentationListener = listener;
    }

    private void onPresented(boolean success) {
        if (presentationListener != null) presentationListener.onPresented(success);
        if (success) {
            surface.setAlpha(1.0f);
            surface.bringToFront();
            if (fallback != null) fallback.setVisibility(View.GONE);
            return;
        }
        surface.setAlpha(0.01f);
        if (imagePath == null) {
            if (missingHandler != null) missingHandler.onMissing();
            return;
        }
        if (fallback == null) {
            fallback = new ImageView(getContext());
            addView(fallback, new LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.MATCH_PARENT));
        }
        fallback.setScaleType(fillView ? ImageView.ScaleType.CENTER_CROP
                                       : ImageView.ScaleType.FIT_CENTER);
        fallback.setVisibility(View.VISIBLE);
        fallback.bringToFront();
        MessageImageLoader.load(imagePath, requestedWidth, requestedHeight, fallback,
                                missingHandler);
    }
}
