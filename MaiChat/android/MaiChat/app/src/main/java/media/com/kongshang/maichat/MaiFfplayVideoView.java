package com.kongshang.maichat;

import android.content.Context;
import android.graphics.Color;
import android.os.Handler;
import android.os.Looper;
import android.view.Gravity;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.SeekBar;
import android.widget.TextView;
import java.lang.ref.WeakReference;

/** Native Graphics surface controlled by the shared in-process ffplay engine. */
public final class MaiFfplayVideoView extends FrameLayout {
    private static final Handler MAIN = new Handler(Looper.getMainLooper());
    private static volatile WeakReference<MaiFfplayVideoView> active = new WeakReference<>(null);

    static { System.loadLibrary("maichat_agent"); }
    private static native long nativeCreate(long viewId, String path);
    private static native boolean nativeCommand(long handle, String command);
    private static native boolean nativeSeekPercent(long handle, double fraction);
    private static native void nativeDestroy(long handle);

    private final MaiGraphicsTextureView surface;
    private final TextView errorLabel;
    private final Button playButton;
    private final SeekBar seekBar;
    private String videoPath;
    private long playbackHandle;
    private boolean playing = true;
    private volatile boolean presented;

    public MaiFfplayVideoView(Context context) {
        super(context);
        setBackgroundColor(Color.BLACK);
        surface = new MaiGraphicsTextureView(context);
        surface.setAlpha(0.01f);
        errorLabel = new TextView(context);
        errorLabel.setText("正在打开视频…");
        errorLabel.setTextColor(Color.WHITE);
        errorLabel.setGravity(Gravity.CENTER);
        surface.setPresentationListener(success -> {
            if (!success) return;
            presented = true;
            surface.setAlpha(1.0f);
            errorLabel.setVisibility(GONE);
        });
        surface.setViewReadyListener(new MaiGraphicsTextureView.ViewReadyListener() {
            @Override public void onReady(long viewId) { start(viewId); }
            @Override public void onLost() { stop(); }
        });
        addView(surface, new LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.MATCH_PARENT));
        addView(errorLabel, new LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.MATCH_PARENT));

        LinearLayout controls = new LinearLayout(context);
        controls.setOrientation(LinearLayout.HORIZONTAL);
        controls.setGravity(Gravity.CENTER_VERTICAL);
        controls.setPadding(12, 0, 12, 0);
        controls.setBackgroundColor(Color.argb(160, 0, 0, 0));
        playButton = new Button(context);
        playButton.setText("暂停");
        playButton.setOnClickListener(view -> command(playing ? "pause" : "play"));
        controls.addView(playButton, new LinearLayout.LayoutParams(90, LayoutParams.WRAP_CONTENT));
        seekBar = new SeekBar(context);
        seekBar.setMax(1000);
        seekBar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar bar, int value, boolean fromUser) {}
            @Override public void onStartTrackingTouch(SeekBar bar) {}
            @Override public void onStopTrackingTouch(SeekBar bar) {
                seekPercent(bar.getProgress() / 1000.0);
            }
        });
        controls.addView(seekBar, new LinearLayout.LayoutParams(0, LayoutParams.WRAP_CONTENT, 1));
        addView(controls, new LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.WRAP_CONTENT,
            Gravity.BOTTOM));
    }

    public static MaiFfplayVideoView activeView() { return active.get(); }
    public boolean hasPresentedFrame() { return presented; }

    public void setVideoPath(String path) {
        videoPath = path;
        if (surface.isAvailable()) startReadySurface();
    }

    private void startReadySurface() {
        // setViewReadyListener calls back immediately when the native surface exists.
        surface.setViewReadyListener(new MaiGraphicsTextureView.ViewReadyListener() {
            @Override public void onReady(long viewId) { start(viewId); }
            @Override public void onLost() { stop(); }
        });
    }

    private void start(long viewId) {
        if (playbackHandle != 0 || videoPath == null || videoPath.isEmpty()) return;
        MaiFfplayVideoView previous = active.get();
        if (previous != null && previous != this) previous.stop();
        playbackHandle = nativeCreate(viewId, videoPath);
        if (playbackHandle == 0) {
            errorLabel.setText("无法打开视频");
            return;
        }
        active = new WeakReference<>(this);
        presented = false;
        MAIN.postDelayed(() -> {
            if (playbackHandle != 0 && !presented)
                errorLabel.setText("视频暂时无法播放");
        }, 8000);
    }

    public boolean command(String action) {
        if (playbackHandle == 0) return false;
        if (action.equals("play")) {
            if (playing) return true;
            action = "pause";
        } else if (action.equals("pause")) {
            if (!playing) return true;
        } else if (action.equals("toggle_pause")) {
            action = "pause";
        }
        boolean accepted = nativeCommand(playbackHandle, action);
        if (accepted && action.equals("pause")) {
            playing = !playing;
            playButton.setText(playing ? "暂停" : "播放");
        }
        return accepted;
    }

    public boolean seekPercent(double fraction) {
        return playbackHandle != 0 && nativeSeekPercent(playbackHandle, fraction);
    }

    public void stop() {
        long handle = playbackHandle;
        playbackHandle = 0;
        if (handle != 0) nativeDestroy(handle);
        if (active.get() == this) active = new WeakReference<>(null);
    }

    @Override protected void onDetachedFromWindow() {
        stop();
        super.onDetachedFromWindow();
    }
}
