// 文件作用：Android 音乐界面与文件导入；解码、时间轴和控制由共享 FFplay 内核执行。
package com.kongshang.maichat;

import android.app.Activity;
import android.app.AlertDialog;
import android.graphics.Color;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.os.Handler;
import android.os.Looper;
import android.view.Gravity;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.SeekBar;
import android.widget.TextView;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.lang.ref.WeakReference;
import java.util.Locale;
import java.util.function.Consumer;

final class LocalMusicPlayer {
    private static WeakReference<LocalMusicPlayer> active = new WeakReference<>(null);
    private final Activity activity;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final Runnable progressTick = this::refresh;
    private MaiFfplayAudioSession session;
    private AlertDialog dialog;
    private SeekBar progress;
    private TextView time, status, title;
    private Button toggle;
    private File currentFile, stagedFile;
    private long generation;
    private boolean playing, seeking;

    LocalMusicPlayer(Activity activity) { this.activity = activity; }

    static void closeActive() {
        LocalMusicPlayer player = active.get();
        if (player != null) player.close();
    }

    boolean open(File file) {
        if (file == null || !file.isFile() || !file.canRead() || file.length() < 1) return false;
        showPlayer(file.getName(), "本地文件");
        return setFile(file);
    }

    void open(Uri uri, String name, String origin, Consumer<Boolean> completed) {
        if (uri == null) { completed.accept(false); return; }
        showPlayer(name, origin);
        final long request = generation;
        new Thread(() -> {
            File copied = null;
            try {
                File folder = new File(activity.getCacheDir(), "MaiChatLocalMusic");
                if (!folder.isDirectory() && !folder.mkdirs())
                    throw new IllegalStateException("无法创建音乐缓存目录");
                copied = File.createTempFile("music-", ".audio", folder);
                try (InputStream input = activity.getContentResolver().openInputStream(uri);
                     FileOutputStream output = new FileOutputStream(copied)) {
                    if (input == null) throw new IllegalStateException("无法读取音乐文件");
                    byte[] buffer = new byte[64 * 1024];
                    long total = 0;
                    for (int count; (count = input.read(buffer)) != -1;) {
                        total += count;
                        if (total > 200_000_000) throw new IllegalStateException("音乐超过 200 MB");
                        output.write(buffer, 0, count);
                    }
                    if (total == 0) throw new IllegalStateException("音乐文件为空");
                }
            } catch (Exception failure) {
                if (copied != null) copied.delete();
                copied = null;
            }
            File ready = copied;
            handler.post(() -> {
                if (generation != request || dialog == null) {
                    if (ready != null) ready.delete();
                    completed.accept(false);
                } else if (ready == null) {
                    status.setText("无法读取所选歌曲");
                    completed.accept(false);
                } else {
                    stagedFile = ready;
                    completed.accept(setFile(ready));
                }
            });
        }, "MaiChatMusicImport").start();
    }

    private void showPlayer(String name, String origin) {
        close();
        LinearLayout body = new LinearLayout(activity);
        body.setOrientation(LinearLayout.VERTICAL);
        body.setGravity(Gravity.CENTER_HORIZONTAL);
        body.setPadding(dp(22), dp(18), dp(22), dp(10));
        TextView source = new TextView(activity);
        source.setText("正在播放 · " + origin);
        source.setTextColor(MaiChatTheme.SECONDARY);
        source.setTextSize(13);
        body.addView(source, new LinearLayout.LayoutParams(-1, -2));
        TextView artwork = new TextView(activity);
        artwork.setText("♪");
        artwork.setTextColor(Color.WHITE);
        artwork.setTextSize(52);
        artwork.setGravity(Gravity.CENTER);
        GradientDrawable cover = new GradientDrawable(GradientDrawable.Orientation.TL_BR,
            new int[] {Color.rgb(33, 147, 238), Color.rgb(99, 78, 194)});
        cover.setCornerRadius(dp(24));
        artwork.setBackground(cover);
        LinearLayout.LayoutParams coverSize = new LinearLayout.LayoutParams(dp(104), dp(104));
        coverSize.setMargins(0, dp(20), 0, dp(16));
        body.addView(artwork, coverSize);
        title = new TextView(activity);
        title.setText(name == null || name.trim().isEmpty() ? "音乐" : name.trim());
        title.setTextColor(MaiChatTheme.TEXT);
        title.setTextSize(18);
        title.setTypeface(MaiChatTypography.semibold());
        title.setGravity(Gravity.CENTER);
        title.setMaxLines(2);
        body.addView(title, new LinearLayout.LayoutParams(-1, -2));
        progress = new SeekBar(activity);
        progress.setMax(1000);
        progress.setEnabled(false);
        LinearLayout.LayoutParams progressSize = new LinearLayout.LayoutParams(-1, -2);
        progressSize.setMargins(0, dp(12), 0, 0);
        body.addView(progress, progressSize);
        time = new TextView(activity);
        time.setText("0:00 / 0:00");
        time.setTextColor(MaiChatTheme.SECONDARY);
        body.addView(time, new LinearLayout.LayoutParams(-1, -2));
        status = new TextView(activity);
        status.setText("正在载入…");
        status.setTextColor(MaiChatTheme.SECONDARY);
        body.addView(status, new LinearLayout.LayoutParams(-1, -2));
        LinearLayout controls = new LinearLayout(activity);
        controls.setGravity(Gravity.CENTER);
        toggle = new Button(activity);
        toggle.setText("播放");
        toggle.setAllCaps(false);
        toggle.setTextColor(Color.WHITE);
        toggle.setBackground(MaiChatTheme.rounded(MaiChatTheme.BLUE, 18, activity));
        Button stop = new Button(activity);
        stop.setText("停止");
        stop.setAllCaps(false);
        stop.setTextColor(MaiChatTheme.BLUE);
        stop.setBackground(MaiChatTheme.rounded(Color.rgb(235, 246, 255), 18, activity));
        LinearLayout.LayoutParams actionSize = new LinearLayout.LayoutParams(dp(96), dp(44));
        actionSize.setMargins(0, 0, dp(12), 0);
        controls.addView(toggle, actionSize);
        controls.addView(stop, new LinearLayout.LayoutParams(dp(96), dp(44)));
        LinearLayout.LayoutParams controlsSize = new LinearLayout.LayoutParams(-1, -2);
        controlsSize.setMargins(0, dp(16), 0, 0);
        body.addView(controls, controlsSize);
        dialog = new AlertDialog.Builder(activity).setView(body)
            .setNegativeButton("关闭", (d, which) -> close()).create();
        dialog.setOnDismissListener(ignored -> close());
        dialog.show();
        active = new WeakReference<>(this);
        toggle.setOnClickListener(view -> {
            if (session == null || session.finishedCode() != Integer.MIN_VALUE) {
                if (currentFile != null) setFile(currentFile);
            } else if (session.pauseToggle()) {
                playing = !playing;
                toggle.setText(playing ? "暂停" : "播放");
            }
        });
        stop.setOnClickListener(view -> {
            stopSession();
            progress.setProgress(0);
            time.setText("0:00 / 0:00");
            toggle.setText("播放");
        });
        progress.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override public void onStartTrackingTouch(SeekBar bar) { seeking = true; }
            @Override public void onProgressChanged(SeekBar bar, int value, boolean fromUser) {}
            @Override public void onStopTrackingTouch(SeekBar bar) {
                seeking = false;
                long[] state = session == null ? null : session.status();
                if (state != null && state[1] > 0)
                    session.seek((long) (state[1] * (bar.getProgress() / 1000.0)));
            }
        });
    }

    private boolean setFile(File file) {
        stopSession();
        MaiFfplayVideoView video = MaiFfplayVideoView.activeView();
        if (video != null) video.stop();
        currentFile = file;
        session = MaiFfplayAudioSession.open(file.getAbsolutePath());
        if (session == null) {
            status.setText("FFplay 音频播放器无法启动");
            return false;
        }
        status.setText("正在载入…");
        progress.setProgress(0);
        progress.setEnabled(false);
        toggle.setText("暂停");
        playing = true;
        handler.post(progressTick);
        return true;
    }

    private void refresh() {
        if (session == null || dialog == null || !dialog.isShowing()) return;
        long[] state = session.status();
        if (state != null && state.length >= 4 && state[1] > 0) {
            long position = Math.max(0, Math.min(state[0], state[1]));
            if (!seeking) progress.setProgress((int) (position * 1000 / state[1]));
            progress.setEnabled(true);
            time.setText(clock(position) + " / " + clock(state[1]));
            status.setText("");
            playing = state[3] == 0;
            toggle.setText(playing ? "暂停" : "播放");
        } else {
            int code = session.finishedCode();
            if (code != Integer.MIN_VALUE) {
                toggle.setText("重播");
                playing = false;
                if (code != 0) status.setText("这首歌无法用 FFplay 播放");
                return;
            }
        }
        handler.removeCallbacks(progressTick);
        handler.postDelayed(progressTick, 250);
    }

    private void stopSession() {
        handler.removeCallbacks(progressTick);
        if (session != null) { session.close(); session = null; }
        playing = false;
    }

    void close() {
        generation++;
        stopSession();
        if (stagedFile != null) { stagedFile.delete(); stagedFile = null; }
        currentFile = null;
        AlertDialog previous = dialog;
        dialog = null;
        if (previous != null && previous.isShowing()) previous.dismiss();
        if (active.get() == this) active.clear();
    }

    private static String clock(long microseconds) {
        long seconds = Math.max(0, microseconds / 1_000_000);
        return String.format(Locale.ROOT, "%d:%02d", seconds / 60, seconds % 60);
    }

    private int dp(int value) {
        return Math.round(value * activity.getResources().getDisplayMetrics().density);
    }
}
