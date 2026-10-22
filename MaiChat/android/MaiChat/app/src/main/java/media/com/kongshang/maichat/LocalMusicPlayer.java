// 文件作用：播放用户选择的本地音乐；创作中心按钮与 Agent 工具共用此播放器。
package com.kongshang.maichat;

import android.app.Activity;
import android.app.AlertDialog;
import android.graphics.Color;
import android.media.MediaPlayer;
import android.net.Uri;
import android.os.Handler;
import android.os.Looper;
import android.view.Gravity;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.SeekBar;
import android.widget.TextView;
import java.io.File;
import java.util.Locale;

final class LocalMusicPlayer {
    private final Activity activity;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final Runnable progressTick = this::refresh;
    private MediaPlayer player;
    private AlertDialog dialog;
    private SeekBar progress;
    private TextView time;
    private TextView status;
    private Button toggle;
    private boolean prepared;

    LocalMusicPlayer(Activity activity) { this.activity = activity; }

    boolean open(Uri uri, String name) {
        return openInternal(uri, null, name);
    }

    boolean open(File file) {
        if (file == null || !file.isFile() || !file.canRead() || file.length() < 1) return false;
        return openInternal(null, file, file.getName());
    }

    private boolean openInternal(Uri uri, File file, String name) {
        if (Looper.myLooper() != Looper.getMainLooper())
            throw new IllegalStateException("Music player must open on the UI thread");
        close();
        try {
            MediaPlayer next = new MediaPlayer();
            if (file != null) next.setDataSource(file.getCanonicalPath());
            else next.setDataSource(activity, uri);
            player = next;
        } catch (Exception error) {
            close();
            return false;
        }
        prepared = false;
        LinearLayout body = new LinearLayout(activity);
        body.setOrientation(LinearLayout.VERTICAL);
        body.setPadding(dp(22), dp(24), dp(22), dp(8));
        TextView title = new TextView(activity);
        title.setText(name == null || name.isEmpty() ? "本地音乐" : name);
        title.setTextSize(18);
        title.setTextColor(MaiChatTheme.TEXT);
        title.setTypeface(MaiChatTypography.semibold());
        title.setMaxLines(2);
        body.addView(title, new LinearLayout.LayoutParams(-1, -2));
        progress = new SeekBar(activity);
        progress.setMax(1000);
        progress.setEnabled(false);
        body.addView(progress, new LinearLayout.LayoutParams(-1, -2));
        time = new TextView(activity);
        time.setText("0:00 / 0:00");
        time.setTextColor(MaiChatTheme.SECONDARY);
        body.addView(time, new LinearLayout.LayoutParams(-1, -2));
        status = new TextView(activity);
        status.setText("正在加载…");
        status.setTextColor(MaiChatTheme.SECONDARY);
        body.addView(status, new LinearLayout.LayoutParams(-1, -2));
        LinearLayout controls = new LinearLayout(activity);
        controls.setGravity(Gravity.CENTER_VERTICAL);
        toggle = new Button(activity);
        toggle.setText("播放");
        toggle.setEnabled(false);
        toggle.setAllCaps(false);
        toggle.setTextColor(Color.WHITE);
        toggle.setBackground(MaiChatTheme.rounded(MaiChatTheme.BLUE, 18, activity));
        Button stop = new Button(activity);
        stop.setText("停止");
        stop.setAllCaps(false);
        stop.setTextColor(MaiChatTheme.BLUE);
        stop.setBackground(MaiChatTheme.rounded(Color.rgb(235, 246, 255), 18, activity));
        LinearLayout.LayoutParams toggleParams = new LinearLayout.LayoutParams(dp(96), dp(44));
        toggleParams.setMargins(0, 0, dp(12), 0);
        controls.addView(toggle, toggleParams);
        controls.addView(stop, new LinearLayout.LayoutParams(dp(96), dp(44)));
        body.addView(controls, new LinearLayout.LayoutParams(-1, -2));
        dialog = new AlertDialog.Builder(activity).setTitle("本地音乐")
            .setView(body).setNegativeButton("关闭", (d, which) -> close()).create();
        dialog.setOnDismissListener(ignored -> close());
        dialog.show();

        final MediaPlayer current = player;
        current.setOnPreparedListener(ready -> {
            if (player != current) return;
            prepared = true;
            progress.setEnabled(true);
            toggle.setEnabled(true);
            status.setText("");
            ready.start();
            toggle.setText("暂停");
            refresh();
        });
        current.setOnCompletionListener(finished -> {
            if (player != current) return;
            toggle.setText("播放");
            refresh();
        });
        current.setOnErrorListener((failed, what, extra) -> {
            if (player == current) {
                status.setText("此文件无法播放");
                toggle.setEnabled(false);
            }
            return true;
        });
        toggle.setOnClickListener(view -> {
            if (!prepared || player != current) return;
            if (current.isPlaying()) current.pause();
            else {
                if (current.getCurrentPosition() >= current.getDuration()) current.seekTo(0);
                current.start();
            }
            toggle.setText(current.isPlaying() ? "暂停" : "播放");
        });
        stop.setOnClickListener(view -> close());
        progress.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            public void onStartTrackingTouch(SeekBar bar) {}
            public void onProgressChanged(SeekBar bar, int value, boolean fromUser) {}
            public void onStopTrackingTouch(SeekBar bar) {
                if (prepared && player == current && current.getDuration() > 0)
                    current.seekTo((int) ((long) bar.getProgress() * current.getDuration() / 1000));
            }
        });
        try { current.prepareAsync(); }
        catch (IllegalStateException failure) { close(); return false; }
        return true;
    }

    void close() {
        handler.removeCallbacksAndMessages(null);
        prepared = false;
        MediaPlayer previous = player;
        player = null;
        if (previous != null) {
            try { previous.reset(); } catch (IllegalStateException ignored) {}
            previous.release();
        }
        AlertDialog old = dialog;
        dialog = null;
        if (old != null && old.isShowing()) old.dismiss();
    }

    private void refresh() {
        if (!prepared || player == null || dialog == null || !dialog.isShowing()) return;
        int duration = Math.max(0, player.getDuration());
        int position = Math.max(0, player.getCurrentPosition());
        if (!progress.isPressed() && duration > 0)
            progress.setProgress((int) ((long) position * 1000 / duration));
        time.setText(clock(position) + " / " + clock(duration));
        handler.removeCallbacks(progressTick);
        handler.postDelayed(progressTick, 250);
    }

    private static String clock(int milliseconds) {
        int seconds = Math.max(0, milliseconds / 1000);
        return String.format(Locale.ROOT, "%d:%02d", seconds / 60, seconds % 60);
    }

    private int dp(int value) {
        return Math.round(value * activity.getResources().getDisplayMetrics().density);
    }
}
