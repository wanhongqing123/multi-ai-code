package com.kongshang.maichat;

import android.content.Context;
import android.graphics.Color;
import android.graphics.Typeface;
import android.content.res.ColorStateList;
import android.os.Handler;
import android.os.Looper;
import android.view.Gravity;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.SeekBar;
import android.widget.TextView;
import java.lang.ref.WeakReference;
import java.util.Locale;

/** Native Graphics surface controlled by the shared in-process ffplay engine. */
public final class MaiFfplayVideoView extends FrameLayout {
    private static final Handler MAIN = new Handler(Looper.getMainLooper());
    private static volatile WeakReference<MaiFfplayVideoView> active = new WeakReference<>(null);

    static { System.loadLibrary("maichat_agent"); }
    private static native long nativeCreate(long viewId, String path);
    private static native boolean nativeCommand(long handle, String command);
    private static native boolean nativeSeekPercent(long handle, double fraction);
    private static native long[] nativePlaybackStatus(long handle);
    private static native void nativeDestroy(long handle);

    private final MaiGraphicsTextureView surface;
    private final TextView errorLabel;
    private final Button playButton;
    private final SeekBar seekBar;
    private final TextView timeLabel;
    private boolean seeking;
    private final Runnable refreshProgress = new Runnable() {
        @Override public void run() {
            if (playbackHandle == 0) return;
            long[] status = nativePlaybackStatus(playbackHandle);
            if (status != null && status.length == 4 && status[1] > 0) {
                long position = Math.max(0, Math.min(status[0], status[1]));
                if (!seeking) seekBar.setProgress((int) (position * 1000 / status[1]));
                timeLabel.setText(timestamp(position) + " / " + timestamp(status[1]));
                playing = status[3] == 0;
                playButton.setText(playing ? "Ⅱ" : "▶");
                playButton.setContentDescription(playing ? "暂停视频" : "播放视频");
            }
            MAIN.postDelayed(this, 500);
        }
    };
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
        controls.setPadding(dp(12), dp(4), dp(12), dp(4));
        controls.setBackgroundColor(Color.argb(125, 0, 0, 0));
        playButton = new Button(context);
        playButton.setText("Ⅱ");
        playButton.setTextSize(27);
        playButton.setTextColor(Color.WHITE);
        playButton.setBackgroundColor(Color.TRANSPARENT);
        playButton.setContentDescription("暂停视频");
        playButton.setMinWidth(0);
        playButton.setMinimumWidth(0);
        playButton.setPadding(0, dp(18), 0, 0);
        playButton.setOnClickListener(view -> command(playing ? "pause" : "play"));
        controls.addView(playButton, new LinearLayout.LayoutParams(dp(56), dp(58)));
        LinearLayout timeline = new LinearLayout(context);
        timeline.setOrientation(LinearLayout.VERTICAL);
        seekBar = new SeekBar(context);
        seekBar.setMax(1000);
        seekBar.setProgressTintList(ColorStateList.valueOf(Color.WHITE));
        seekBar.setThumbTintList(ColorStateList.valueOf(Color.WHITE));
        seekBar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar bar, int value, boolean fromUser) {}
            @Override public void onStartTrackingTouch(SeekBar bar) { seeking = true; }
            @Override public void onStopTrackingTouch(SeekBar bar) {
                seeking = false;
                seekPercent(bar.getProgress() / 1000.0);
            }
        });
        timeLabel = new TextView(context);
        timeLabel.setText("0:00 / 0:00");
        timeLabel.setTextColor(Color.WHITE);
        timeLabel.setTextSize(14);
        timeLabel.setTypeface(Typeface.MONOSPACE);
        timeline.addView(timeLabel);
        timeline.addView(seekBar, new LinearLayout.LayoutParams(
            LayoutParams.MATCH_PARENT, LayoutParams.WRAP_CONTENT));
        controls.addView(timeline, new LinearLayout.LayoutParams(0, dp(64), 1));
        addView(controls, new LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.WRAP_CONTENT,
            Gravity.BOTTOM));
    }

    private static String timestamp(long microseconds) {
        long seconds = Math.max(0, microseconds / 1_000_000);
        return String.format(Locale.US, "%d:%02d", seconds / 60, seconds % 60);
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
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
        MAIN.removeCallbacks(refreshProgress);
        MAIN.post(refreshProgress);
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
            playButton.setText(playing ? "Ⅱ" : "▶");
            playButton.setContentDescription(playing ? "暂停视频" : "播放视频");
        }
        return accepted;
    }

    public boolean seekPercent(double fraction) {
        return playbackHandle != 0 && nativeSeekPercent(playbackHandle, fraction);
    }

    public void stop() {
        MAIN.removeCallbacks(refreshProgress);
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
