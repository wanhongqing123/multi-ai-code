// 文件作用：Android 媒体播放、预览和编辑；MaiFfplayVideoView 负责界面呈现、交互与状态更新，本文件给出其实现。
// 类型关系：MaiFfplayVideoView extends FrameLayout；审阅时可据此追踪父类、接口或回调契约。
// 主要入口：nativeCommand、nativeSeekPercent、nativeSeekAbsoluteUs、nativePlaybackStatus。
package com.kongshang.maichat;

import android.content.Context;
import android.graphics.Color;
import android.graphics.Bitmap;
import android.graphics.Typeface;
import android.content.res.ColorStateList;
import android.os.Handler;
import android.os.Looper;
import android.view.Gravity;
import android.view.MotionEvent;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.ImageButton;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.SeekBar;
import android.widget.TextView;
import java.lang.ref.WeakReference;
import java.util.Locale;

/** Native Graphics surface controlled by the shared in-process ffplay engine. */
public final class MaiFfplayVideoView extends FrameLayout {
    public interface OnVideoSwipeListener {
        void onVideoSwipe(int direction);
    }
    private static final Handler MAIN = new Handler(Looper.getMainLooper());
    private static volatile WeakReference<MaiFfplayVideoView> active = new WeakReference<>(null);

    static { System.loadLibrary("maichat_agent"); }
    private static native long nativeCreate(long viewId, String path, double speed,
                                            long startPositionUs, long vodHandle);
    private static native boolean nativeCommand(long handle, String command);
    private static native boolean nativeSeekPercent(long handle, double fraction);
    private static native boolean nativeSeekAbsoluteUs(long handle, long positionUs);
    private static native long[] nativePlaybackStatus(long handle);
    private static native void nativeDestroy(long handle);

    private final MaiGraphicsTextureView surface;
    private long cloudVodHandle;
    private final TextView errorLabel;
    private final ImageButton playButton;
    private final Button speedButton;
    private final LinearLayout speedChoices;
    private final FrameLayout.LayoutParams controlsLayout;
    private final FrameLayout.LayoutParams speedChoicesLayout;
    private final SeekBar seekBar;
    private final TextView timeLabel;
    private boolean seeking;
    private final Runnable refreshProgress = new Runnable() {
        @Override public void run() {
            if (playbackHandle == 0) return;
            long[] status = nativePlaybackStatus(playbackHandle);
            if (status != null && status.length >= 4 && status[1] > 0) {
                long position = Math.max(0, Math.min(status[0], status[1]));
                if (!seeking) seekBar.setProgress((int) (position * 1000 / status[1]));
                timeLabel.setText(timestamp(position) + " / " + timestamp(status[1]));
                playing = status[3] == 0;
                playButton.setImageResource(playing ? android.R.drawable.ic_media_pause
                    : android.R.drawable.ic_media_play);
                playButton.setContentDescription(playing ? "暂停视频" : "播放视频");
                if (status.length >= 5) {
                    speed = status[4] / 100.0;
                    speedButton.setText(formatSpeed(speed));
                }
            }
            MAIN.postDelayed(this, 500);
        }
    };
    private String videoPath;
    private long startPositionUs;
    private long graphicsViewId;
    private long playbackHandle;
    private double speed = 1.0;
    private boolean playing = true;
    private volatile boolean presented;
    private OnVideoSwipeListener swipeListener;
    private float touchStartX, touchStartY;

    // 作用：创建 FFplay 播放控件并连接按钮、进度条和原生句柄。
    public MaiFfplayVideoView(Context context) {
        super(context);
        setBackgroundColor(Color.BLACK);
        surface = new MaiGraphicsTextureView(context);
        surface.setAlpha(0.01f);
        surface.setOnTouchListener((view, event) -> {
            if (event.getActionMasked() == MotionEvent.ACTION_DOWN) {
                touchStartX = event.getX();
                touchStartY = event.getY();
                return true;
            }
            if (event.getActionMasked() == MotionEvent.ACTION_UP) {
                float dx = event.getX() - touchStartX;
                float dy = event.getY() - touchStartY;
                if (Math.abs(dx) >= dp(72) && Math.abs(dx) > Math.abs(dy) * 1.4f) {
                    if (swipeListener != null) swipeListener.onVideoSwipe(dx < 0 ? 1 : -1);
                }
                return true;
            }
            return true;
        });
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
        controls.setBackground(MaiChatTheme.rounded(Color.argb(190, 9, 15, 25), 16, context));
        playButton = new ImageButton(context);
        playButton.setImageResource(android.R.drawable.ic_media_pause);
        playButton.setImageTintList(ColorStateList.valueOf(Color.WHITE));
        playButton.setScaleType(ImageView.ScaleType.CENTER_INSIDE);
        playButton.setBackgroundColor(Color.TRANSPARENT);
        playButton.setContentDescription("暂停视频");
        playButton.setMinimumWidth(0);
        playButton.setPadding(dp(15), dp(15), dp(15), dp(15));
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
        speedButton = new Button(context);
        speedButton.setText("1×");
        speedButton.setTextSize(14);
        speedButton.setTextColor(Color.WHITE);
        speedButton.setBackground(MaiChatTheme.rounded(
            Color.argb(45, 255, 255, 255), 24, context));
        speedButton.setContentDescription("播放倍速");
        controls.addView(speedButton, new LinearLayout.LayoutParams(dp(64), dp(58)));
        controlsLayout = new LayoutParams(LayoutParams.MATCH_PARENT,
            LayoutParams.WRAP_CONTENT, Gravity.BOTTOM);
        controlsLayout.setMargins(dp(16), 0, dp(16), dp(4));
        addView(controls, controlsLayout);

        speedChoices = new LinearLayout(context);
        speedButton.setOnClickListener(view -> speedChoices.setVisibility(
            speedChoices.getVisibility() == VISIBLE ? GONE : VISIBLE));
        speedChoices.setGravity(Gravity.CENTER_VERTICAL);
        speedChoices.setPadding(dp(6), dp(5), dp(6), dp(5));
        speedChoices.setBackground(MaiChatTheme.rounded(
            Color.argb(220, 9, 15, 25), 16, context));
        final double[] rates = {0.25, 0.5, 1.0, 1.5, 2.0};
        for (double rate : rates) {
            Button choice = new Button(context);
            choice.setText(formatSpeed(rate));
            choice.setAllCaps(false);
            choice.setTextSize(14);
            choice.setTextColor(Color.WHITE);
            choice.setBackgroundColor(Color.TRANSPARENT);
            choice.setContentDescription("播放倍速 " + formatSpeed(rate));
            choice.setOnClickListener(view -> {
                speedChoices.setVisibility(GONE);
                if (!setSpeed(rate)) {
                    errorLabel.setText("倍速切换失败");
                    errorLabel.setVisibility(VISIBLE);
                }
            });
            speedChoices.addView(choice, new LinearLayout.LayoutParams(0, dp(44), 1));
        }
        speedChoices.setVisibility(GONE);
        speedChoicesLayout = new LayoutParams(LayoutParams.MATCH_PARENT,
            LayoutParams.WRAP_CONTENT, Gravity.BOTTOM);
        speedChoicesLayout.setMargins(dp(16), 0, dp(16), dp(74));
        addView(speedChoices, speedChoicesLayout);
    }

    public void setControlsBottomInset(int bottomDp) {
        controlsLayout.bottomMargin = dp(Math.max(0, bottomDp) + 4);
        speedChoicesLayout.bottomMargin = dp(Math.max(0, bottomDp) + 74);
        requestLayout();
    }

    private static String timestamp(long microseconds) {
        long seconds = Math.max(0, microseconds / 1_000_000);
        return String.format(Locale.US, "%d:%02d", seconds / 60, seconds % 60);
    }

    private static String formatSpeed(double rate) {
        return rate == 1.0 ? "1×" : String.format(Locale.US, "%s×", rate);
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    public static MaiFfplayVideoView activeView() { return active.get(); }
    public boolean hasPresentedFrame() { return presented; }
    public Bitmap snapshot() {
        return presented && surface.isAvailable() ? surface.getBitmap() : null;
    }
    public String currentVideoPath() { return videoPath; }
    public void setCloudVodHandle(long handle) { cloudVodHandle = handle; }
    public void setOnVideoSwipeListener(OnVideoSwipeListener listener) {
        swipeListener = listener;
    }

    public void setVideoPath(String path) {
        setVideoPath(path, 0);
    }

    public void setVideoPath(String path, long startPositionUs) {
        if (playbackHandle != 0 && !java.util.Objects.equals(videoPath, path)) stop();
        videoPath = path;
        this.startPositionUs = Math.max(0, startPositionUs);
        presented = false;
        surface.setAlpha(0.01f);
        errorLabel.setText("正在打开视频…");
        errorLabel.setVisibility(VISIBLE);
        if (surface.isAvailable()) startReadySurface();
    }

    private void startReadySurface() {
        // 原生画面表面已存在时，setViewReadyListener 会立即回调。
        surface.setViewReadyListener(new MaiGraphicsTextureView.ViewReadyListener() {
            @Override public void onReady(long viewId) { start(viewId); }
            @Override public void onLost() { stop(); }
        });
    }

    private void start(long viewId) {
        if (playbackHandle != 0 || videoPath == null || videoPath.isEmpty()) return;
        LocalMusicPlayer.closeActive();
        graphicsViewId = viewId;
        MaiFfplayVideoView previous = active.get();
        if (previous != null && previous != this) previous.stop();
        playbackHandle = nativeCreate(viewId, videoPath, speed, startPositionUs,
                                      cloudVodHandle);
        if (playbackHandle == 0) {
            errorLabel.setText("无法打开视频");
            return;
        }
        active = new WeakReference<>(this);
        presented = false;
        playing = true;
        playButton.setImageResource(android.R.drawable.ic_media_pause);
        playButton.setContentDescription("暂停视频");
        seekBar.setProgress(0);
        timeLabel.setText("0:00 / 0:00");
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
            playButton.setImageResource(playing ? android.R.drawable.ic_media_pause
                : android.R.drawable.ic_media_play);
            playButton.setContentDescription(playing ? "暂停视频" : "播放视频");
        }
        return accepted;
    }

    public boolean seekPercent(double fraction) {
        return playbackHandle != 0 && nativeSeekPercent(playbackHandle, fraction);
    }

    public boolean seekSeconds(double seconds) {
        if (!Double.isFinite(seconds) || seconds < 0 ||
            seconds > Long.MAX_VALUE / 1_000_000.0 || playbackHandle == 0) return false;
        long[] status = nativePlaybackStatus(playbackHandle);
        if (status == null || status.length < 2 || status[1] <= 0 ||
            seconds * 1_000_000 > status[1]) return false;
        return nativeSeekAbsoluteUs(playbackHandle, Math.round(seconds * 1_000_000));
    }

    public boolean replay() {
        if (graphicsViewId == 0 || videoPath == null || videoPath.isEmpty()) return false;
        long previous = playbackHandle;
        playbackHandle = 0;
        if (previous != 0) nativeDestroy(previous);
        startPositionUs = 0;
        playbackHandle = nativeCreate(graphicsViewId, videoPath, speed, 0,
                                      cloudVodHandle);
        if (playbackHandle == 0) return false;
        playing = true;
        playButton.setImageResource(android.R.drawable.ic_media_pause);
        playButton.setContentDescription("暂停视频");
        seekBar.setProgress(0);
        return true;
    }

    public boolean setSpeed(double requested) {
        if (requested != 0.25 && requested != 0.5 && requested != 1.0 &&
            requested != 1.5 && requested != 2.0) return false;
        if (playbackHandle == 0 || graphicsViewId == 0) return false;
        if (requested == speed) return true;
        long[] status = nativePlaybackStatus(playbackHandle);
        long position = status == null ? 0 : Math.max(0, status[0]);
        boolean wasPlaying = playing;
        long previous = playbackHandle;
        playbackHandle = 0;
        nativeDestroy(previous);
        speed = requested;
        speedButton.setText(formatSpeed(speed));
        playbackHandle = nativeCreate(graphicsViewId, videoPath, speed, position,
                                      cloudVodHandle);
        if (playbackHandle == 0) {
            errorLabel.setText("倍速切换失败");
            errorLabel.setVisibility(VISIBLE);
            return false;
        }
        playing = true;
        if (!wasPlaying) MAIN.postDelayed(() -> command("pause"), 150);
        return true;
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
