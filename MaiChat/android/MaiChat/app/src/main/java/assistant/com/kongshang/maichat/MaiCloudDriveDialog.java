// Private media cloud presentation; network and multipart state live in MaiCloudDriveClient.
package com.kongshang.maichat;

import android.app.AlertDialog;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Color;
import android.net.Uri;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;
import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Locale;
import java.util.UUID;
import org.json.JSONArray;
import org.json.JSONObject;

final class MaiCloudDriveDialog {
    private static final long MAX_THUMBNAIL_BYTES = 2L * 1024 * 1024;
    private static final class CappedScrollView extends ScrollView {
        private final int maximumHeight;
        CappedScrollView(MainActivity activity, int maximumHeight) {
            super(activity);
            this.maximumHeight = maximumHeight;
        }
        @Override protected void onMeasure(int widthSpec, int heightSpec) {
            int allowed = View.MeasureSpec.getMode(heightSpec) == View.MeasureSpec.UNSPECIFIED
                ? maximumHeight : Math.min(View.MeasureSpec.getSize(heightSpec), maximumHeight);
            super.onMeasure(widthSpec,
                View.MeasureSpec.makeMeasureSpec(allowed, View.MeasureSpec.AT_MOST));
        }
    }

    private final MainActivity activity;
    private final MaiCloudDrive client;
    private final MaiChatAttachmentDownload.NativeTransport downloads;
    private MaiChatAttachmentDownload.Scope downloadScope = new MaiChatAttachmentDownload.Scope();
    private final ArrayList<JSONObject> media = new ArrayList<>();
    private AlertDialog dialog;
    private LinearLayout page, uploads, library;
    private TextView quota, status;
    private Button upload, next;
    private ProgressBar progress;
    private String cursor = "";
    private String account = "";
    private final String thumbnailInstance = UUID.randomUUID().toString();
    private File thumbnailDirectory;
    private boolean uploading, loading, playing;
    private int generation;
    private File requestedCheckpoint;

    MaiCloudDriveDialog(MainActivity activity) {
        this.activity = activity;
        this.client = new MaiCloudDrive(activity);
        this.downloads = new MaiChatAttachmentDownload.NativeTransport(client::trustPem);
    }

    boolean isShowing() { return dialog != null && dialog.isShowing(); }

    void dismiss() { if (isShowing()) dialog.dismiss(); }

    void show() {
        account = MaiChatAuthClient.shared(activity).accountId();
        if (account.isEmpty()) {
            Toast.makeText(activity, "请先登录 MaiChat", Toast.LENGTH_SHORT).show();
            return;
        }
        thumbnailDirectory = new File(activity.getCacheDir(),
            "cloud-drive-thumbnails/" + MaiCloudDrive.accountKey(account));
        clearThumbnails();
        page = new LinearLayout(activity);
        page.setOrientation(LinearLayout.VERTICAL);
        page.setBackgroundColor(Color.WHITE);
        page.setPadding(dp(20), dp(18), dp(20), dp(24));
        TextView title = label("我的云盘", 21, MaiChatTheme.TEXT);
        title.setTypeface(MaiChatTypography.semibold());
        page.addView(title, row());
        TextView description = label("仅自己可见 · 视频最多 2 GB · 图片最多 20 MB", 13,
            MaiChatTheme.SECONDARY);
        description.setPadding(0, dp(4), 0, dp(14));
        page.addView(description, row());
        quota = label("正在读取空间…", 13, MaiChatTheme.SECONDARY);
        page.addView(quota, row());
        LinearLayout actions = new LinearLayout(activity);
        actions.setGravity(Gravity.CENTER_VERTICAL);
        actions.setPadding(0, dp(12), 0, dp(8));
        page.addView(actions, row());
        upload = button("上传图片或视频", () -> activity.pickCloudDriveMedia(this));
        actions.addView(upload, new LinearLayout.LayoutParams(0, dp(44), 1));
        Button close = button("关闭", () -> dialog.dismiss());
        LinearLayout.LayoutParams closeParams = new LinearLayout.LayoutParams(dp(74), dp(44));
        closeParams.leftMargin = dp(10);
        actions.addView(close, closeParams);
        status = label("", 13, MaiChatTheme.SECONDARY);
        status.setContentDescription("云盘状态");
        page.addView(status, row());
        progress = new ProgressBar(activity, null,
            android.R.attr.progressBarStyleHorizontal);
        progress.setMax(1000);
        progress.setVisibility(View.GONE);
        LinearLayout.LayoutParams progressParams = row();
        progressParams.topMargin = dp(8);
        page.addView(progress, progressParams);
        ScrollView scroll = new CappedScrollView(activity,
            (int) (activity.getResources().getDisplayMetrics().heightPixels * .56f));
        LinearLayout list = new LinearLayout(activity);
        list.setOrientation(LinearLayout.VERTICAL);
        scroll.addView(list);
        uploads = new LinearLayout(activity);
        uploads.setOrientation(LinearLayout.VERTICAL);
        list.addView(uploads, row());
        list.addView(sectionTitle("我的文件"), row());
        library = new LinearLayout(activity);
        library.setOrientation(LinearLayout.VERTICAL);
        list.addView(library, row());
        next = button("加载更多", this::loadNext);
        next.setVisibility(View.GONE);
        list.addView(next, row());
        page.addView(scroll, row());
        dialog = new AlertDialog.Builder(activity).setView(page).create();
        dialog.setOnDismissListener(ignored -> {
            ++generation;
            client.cancel();
            downloadScope.cancel();
            cleanupIdlePlaybackCache();
            clearThumbnails();
            if (activity.currentCloudDriveDialog() == this) activity.clearCloudDriveDialog(this);
        });
        dialog.show();
        cleanupIdlePlaybackCache();
        activity.cleanupIdleCloudDriveImages();
        Window window = dialog.getWindow();
        if (window != null) {
            window.setBackgroundDrawableResource(android.R.color.transparent);
            window.setLayout(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
            window.setGravity(Gravity.BOTTOM);
            window.addFlags(WindowManager.LayoutParams.FLAG_DIM_BEHIND);
        }
        refresh();
    }

    void confirmAgentFile(File file) {
        try {
            MaiCloudDrive.Source source = client.inspect(file);
            new AlertDialog.Builder(activity).setTitle("保存到我的云盘")
                .setMessage(source.name + "\n" + sizeText(source.bytes) + " · 仅自己可见")
                .setNegativeButton("取消", null)
                .setPositiveButton("保存", (ignored, which) -> upload(source, null))
                .show();
        } catch (IOException failure) { status.setText(failure.getMessage()); }
    }

    void onPicked(Uri uri) {
        if (!isShowing() || uri == null) return;
        try {
            MaiCloudDrive.Source source = client.inspect(uri);
            File checkpoint = requestedCheckpoint;
            requestedCheckpoint = null;
            new AlertDialog.Builder(activity).setTitle(checkpoint == null ?
                    "上传到我的云盘" : "继续上传")
                .setMessage(source.name + "\n" + sizeText(source.bytes) + " · 仅自己可见")
                .setNegativeButton("取消", null)
                .setPositiveButton("上传", (ignored, which) -> upload(source, checkpoint))
                .show();
        } catch (IOException failure) {
            requestedCheckpoint = null;
            status.setText(failure.getMessage());
        }
    }

    void onPickCanceled() { requestedCheckpoint = null; }

    void onPickPermissionFailure() {
        requestedCheckpoint = null;
        status.setText("该文件来源不支持续传授权，请从系统文件管理器选择");
    }

    private void refresh() {
        media.clear();
        cursor = "";
        library.removeAllViews();
        next.setVisibility(View.GONE);
        loadNext();
    }

    private void loadNext() {
        if (loading || uploading) return;
        loading = true;
        int requestGeneration = ++generation;
        status.setText("正在读取云盘…");
        JSONObject payload = new JSONObject();
        try { if (!cursor.isEmpty()) payload.put("before", cursor); }
        catch (Exception ignored) { }
        client.request(MaiCloudDrive.LIST, payload, result -> {
            if (!live(requestGeneration)) return;
            loading = false;
            if (!result.ok()) {
                status.setText(MaiCloudDrive.errorText(result));
                upload.setEnabled(result.httpStatus != 404);
                next.setText("重试");
                next.setVisibility(result.httpStatus == 404 ? View.GONE : View.VISIBLE);
                library.removeAllViews();
                library.addView(label(result.httpStatus == 404 ?
                    "云盘暂未开放" : "暂时无法读取文件，请稍后重试", 14,
                    MaiChatTheme.SECONDARY), row());
                loadUploads(requestGeneration);
                return;
            }
            upload.setEnabled(true);
            JSONObject body = result.json();
            JSONObject usage = body.optJSONObject("quota");
            if (usage != null) {
                long used = usage.optLong("used_bytes");
                long reserved = usage.optLong("reserved_bytes");
                long limit = usage.optLong("limit_bytes", MaiCloudDrive.QUOTA_BYTES);
                quota.setText("已使用 " + sizeText(used + reserved) + " / " + sizeText(limit));
            }
            JSONArray page = body.optJSONArray("videos");
            if (page != null) for (int i = 0; i < page.length(); ++i) {
                JSONObject item = page.optJSONObject(i);
                if (item != null) media.add(item);
            }
            cursor = body.optString("next_cursor", "");
            if ("null".equals(cursor)) cursor = "";
            next.setText("加载更多");
            next.setVisibility(cursor.isEmpty() ? View.GONE : View.VISIBLE);
            renderMedia(requestGeneration);
            status.setText("");
            loadUploads(requestGeneration);
        });
    }

    private void loadUploads(int requestGeneration) {
        client.checkpoints(result -> {
            if (!live(requestGeneration)) return;
            uploads.removeAllViews();
            if (!result.ok()) return;
            JSONArray paused = result.json().optJSONArray("uploads");
            if (paused == null || paused.length() == 0) return;
            uploads.addView(sectionTitle("待续传"), row());
            for (int i = 0; i < paused.length(); ++i) {
                JSONObject upload = paused.optJSONObject(i);
                if (upload != null) uploads.addView(uploadRow(upload), row());
            }
        });
    }

    private View uploadRow(JSONObject item) {
        String name = item.optString("name", "未完成文件");
        File checkpoint = new File(item.optString("checkpoint_path", ""));
        String sourceId = item.optString("source_id", "");
        LinearLayout card = card();
        card.addView(label(name, 15, MaiChatTheme.TEXT), row());
        card.addView(label(sizeText(item.optLong("size_bytes")) + " · 上传尚未完成",
            12, MaiChatTheme.SECONDARY), row());
        LinearLayout actions = new LinearLayout(activity);
        card.addView(actions, row());
        Button resume = button("继续上传", () -> {
            if (sourceId.startsWith("content://")) {
                try {
                    upload(client.inspect(Uri.parse(sourceId)), checkpoint);
                } catch (Exception unavailable) {
                    requestedCheckpoint = checkpoint;
                    status.setText("原文件授权已失效，请重新选择同一文件");
                    activity.pickCloudDriveMedia(this);
                }
            } else {
                File sourceFile = new File(sourceId);
                if (sourceFile.isFile() && sourceFile.length() == item.optLong("size_bytes")) {
                    upload(new MaiCloudDrive.Source(null, sourceFile,
                        item.optString("name", ""),
                        item.optString("content_type", ""),
                        item.optLong("size_bytes")), checkpoint);
                } else {
                    requestedCheckpoint = checkpoint;
                    status.setText("原文件已移走，请重新选择同一文件");
                    activity.pickCloudDriveMedia(this);
                }
            }
        });
        actions.addView(resume, new LinearLayout.LayoutParams(0, dp(42), 1));
        Button discard = button("放弃", () -> new AlertDialog.Builder(activity)
            .setMessage("放弃后会删除未完成的上传分片。原文件不会删除。")
            .setNegativeButton("取消", null)
            .setPositiveButton("放弃上传", (ignored, which) -> {
                status.setText("正在取消上传…");
                client.abort(checkpoint, result -> {
                    status.setText(result.ok() ? "已放弃上传" : MaiCloudDrive.errorText(result));
                    if (result.ok()) {
                        new File(checkpoint.getAbsolutePath() + ".source").delete();
                        refresh();
                    }
                });
            }).show());
        LinearLayout.LayoutParams discardParams = new LinearLayout.LayoutParams(dp(72), dp(42));
        discardParams.leftMargin = dp(8);
        actions.addView(discard, discardParams);
        return card;
    }

    private void upload(MaiCloudDrive.Source source, File resumeCheckpoint) {
        if (uploading || playing) return;
        try {
            File checkpoint = resumeCheckpoint == null ? client.newCheckpoint() : resumeCheckpoint;
            uploading = true;
            upload.setEnabled(false);
            progress.setVisibility(View.VISIBLE);
            progress.setProgress(0);
            status.setText("准备上传 " + source.name + "…");
            Button pause = button("暂停上传", () -> {
                client.cancel();
                status.setText("已暂停，可稍后续传");
                uploading = false;
                upload.setEnabled(true);
                progress.setVisibility(View.GONE);
                refresh();
            });
            uploads.removeAllViews();
            uploads.addView(pause, row());
            client.upload(source, checkpoint, (done, total, preparing) -> {
                if (!isShowing() || !account.equals(MaiChatAuthClient.shared(activity).accountId()))
                    return;
                progress.setProgress(total <= 0 ? 0 :
                    (int) Math.min(1000, done * 1000 / total));
                status.setText((preparing ? "正在准备文件 " : "正在上传 ") +
                    sizeText(done) + " / " + sizeText(total));
            }, result -> {
                if (!isShowing()) return;
                uploading = false;
                progress.setVisibility(View.GONE);
                upload.setEnabled(true);
                if (result.ok()) {
                    new File(checkpoint.getAbsolutePath() + ".source").delete();
                    status.setText("已保存到我的云盘");
                    Toast.makeText(activity, "上传完成", Toast.LENGTH_SHORT).show();
                } else status.setText(MaiCloudDrive.errorText(result));
                refresh();
            });
        } catch (IOException failure) { status.setText(failure.getMessage()); }
    }

    private void renderMedia(int requestGeneration) {
        library.removeAllViews();
        if (media.isEmpty()) {
            TextView empty = label("还没有文件，点上方「上传图片或视频」保存到私人云盘。", 14,
                MaiChatTheme.SECONDARY);
            empty.setPadding(dp(2), dp(22), dp(2), 0);
            library.addView(empty, row());
            return;
        }
        for (JSONObject item : media) library.addView(mediaRow(item, requestGeneration), row());
    }

    private View mediaRow(JSONObject item, int requestGeneration) {
        String id = item.optString("id", item.optString("video_id", ""));
        String name = item.optString("name", "文件");
        long bytes = item.optLong("size_bytes");
        boolean image = "image".equals(item.optString("media_type", "video"));
        LinearLayout card = card();
        LinearLayout content = new LinearLayout(activity);
        content.setGravity(Gravity.CENTER_VERTICAL);
        card.addView(content, row());
        FrameLayout cover = new FrameLayout(activity);
        cover.setBackground(MaiChatTheme.rounded(MaiChatTheme.BLUE_SOFT, 10, activity));
        TextView placeholder = label(image ? "图片" : "视频", 12, MaiChatTheme.BLUE_DARK);
        placeholder.setGravity(Gravity.CENTER);
        cover.addView(placeholder, new FrameLayout.LayoutParams(-1, -1));
        ImageView thumbnail = new ImageView(activity);
        thumbnail.setScaleType(ImageView.ScaleType.CENTER_CROP);
        thumbnail.setContentDescription(name + "缩略图");
        cover.addView(thumbnail, new FrameLayout.LayoutParams(-1, -1));
        cover.setOnClickListener(ignored -> open(id, name, bytes, image));
        content.addView(cover, new LinearLayout.LayoutParams(dp(76), dp(76)));
        LinearLayout details = new LinearLayout(activity);
        details.setOrientation(LinearLayout.VERTICAL);
        LinearLayout.LayoutParams detailParams = new LinearLayout.LayoutParams(0, -2, 1);
        detailParams.leftMargin = dp(12);
        content.addView(details, detailParams);
        TextView filename = label(name, 15, MaiChatTheme.TEXT);
        filename.setSingleLine(true);
        filename.setEllipsize(android.text.TextUtils.TruncateAt.MIDDLE);
        details.addView(filename, row());
        details.addView(label(sizeText(bytes) + " · " + (image ? "图片" : "视频") +
            " · 仅自己可见", 12, MaiChatTheme.SECONDARY), row());
        loadThumbnail(id, thumbnail, placeholder, requestGeneration);
        LinearLayout actions = new LinearLayout(activity);
        card.addView(actions, row());
        Button open = button(image ? "查看" : "播放", () -> open(id, name, bytes, image));
        actions.addView(open, new LinearLayout.LayoutParams(0, dp(42), 1));
        Button delete = button("删除", () -> new AlertDialog.Builder(activity)
            .setMessage("删除「" + name + "」？此操作无法撤销。")
            .setNegativeButton("取消", null)
            .setPositiveButton("删除", (ignored, which) -> delete(id)).show());
        LinearLayout.LayoutParams deleteParams = new LinearLayout.LayoutParams(dp(72), dp(42));
        deleteParams.leftMargin = dp(8);
        actions.addView(delete, deleteParams);
        return card;
    }

    private void loadThumbnail(String id, ImageView thumbnail, TextView placeholder,
                               int requestGeneration) {
        if (id.isEmpty()) return;
        if (thumbnailDirectory == null) {
            thumbnailDirectory = new File(activity.getCacheDir(),
                "cloud-drive-thumbnails/" + MaiCloudDrive.accountKey(account));
        }
        if (!thumbnailDirectory.isDirectory() && !thumbnailDirectory.mkdirs()) return;
        File target = new File(thumbnailDirectory,
            UUID.nameUUIDFromBytes(id.getBytes(StandardCharsets.UTF_8)) +
                "-" + thumbnailInstance + ".jpg");
        if (target.isFile()) {
            Bitmap cached = decodeThumbnail(target);
            if (cached != null) {
                thumbnail.setImageBitmap(cached);
                placeholder.setVisibility(View.GONE);
                return;
            }
            target.delete();
        }
        JSONObject payload = new JSONObject();
        try { payload.put("video_id", id); } catch (Exception ignored) { return; }
        // A thumbnail has its own request owner. Listing, uploading, or opening
        // another row must not cancel this request's short-lived signed link.
        new MaiCloudDrive(activity).request(MaiCloudDrive.THUMBNAIL_LINK, payload, result -> {
            if (!live(requestGeneration) || !result.ok()) return;
            String url = result.json().optString("thumbnail_url", "");
            if (!url.startsWith("https://")) return;
            MaiChatAttachmentDownload.Scope scope = downloadScope;
            downloads.download(url, target, MAX_THUMBNAIL_BYTES, scope, downloaded -> {
                Bitmap bitmap = downloaded.ok() ? decodeThumbnail(target) : null;
                activity.runOnUiThread(() -> {
                    if (!live(requestGeneration) || scope != downloadScope ||
                        thumbnail.getParent() == null) {
                        if (bitmap != null) bitmap.recycle();
                        target.delete();
                        return;
                    }
                    if (bitmap == null) { target.delete(); return; }
                    thumbnail.setImageBitmap(bitmap);
                    placeholder.setVisibility(View.GONE);
                });
            });
        });
    }

    private static Bitmap decodeThumbnail(File file) {
        BitmapFactory.Options bounds = new BitmapFactory.Options();
        bounds.inJustDecodeBounds = true;
        BitmapFactory.decodeFile(file.getAbsolutePath(), bounds);
        if (bounds.outWidth <= 0 || bounds.outHeight <= 0 ||
            bounds.outWidth > 8192 || bounds.outHeight > 8192) return null;
        BitmapFactory.Options sampled = new BitmapFactory.Options();
        sampled.inSampleSize = 1;
        while (bounds.outWidth / sampled.inSampleSize > 320 ||
               bounds.outHeight / sampled.inSampleSize > 320)
            sampled.inSampleSize *= 2;
        return BitmapFactory.decodeFile(file.getAbsolutePath(), sampled);
    }

    private void clearThumbnails() {
        if (thumbnailDirectory == null) return;
        File[] files = thumbnailDirectory.listFiles();
        if (files != null) for (File file : files) if (file.isFile()) file.delete();
        thumbnailDirectory.delete();
    }

    private void open(String id, String name, long bytes, boolean image) {
        if (id.isEmpty() || playing || uploading) return;
        MaiCloudDrive.MediaInfo info = MaiCloudDrive.mediaInfo(name);
        if (info == null || !info.mediaType.equals(image ? "image" : "video") ||
            bytes < 1 || bytes > info.maxBytes) {
            status.setText("云盘文件信息无效，无法预览");
            return;
        }
        File directory = new File(activity.getCacheDir(), image ?
            "cloud-drive-images" : "cloud-drive-playback");
        if (!directory.isDirectory() && !directory.mkdirs()) {
            status.setText("无法准备预览缓存"); return;
        }
        if (image) activity.cleanupIdleCloudDriveImages();
        else cleanupIdlePlaybackCache();
        if (!MaiCloudDrivePlaybackCache.hasSpace(directory, bytes)) {
            status.setText("设备剩余空间不足，无法缓存文件"); return;
        }
        playing = true;
        status.setText("正在获取预览链接…");
        JSONObject payload = new JSONObject();
        try { payload.put("video_id", id); } catch (Exception ignored) { }
        client.request(MaiCloudDrive.PLAY_LINK, payload, result -> {
            if (!result.ok()) {
                playing = false;
                status.setText(MaiCloudDrive.errorText(result));
                return;
            }
            String url = result.json().optString("read_url", "");
            if (!url.startsWith("https://")) {
                playing = false; status.setText("预览链接无效"); return;
            }
            String extension = name.substring(name.lastIndexOf('.')).toLowerCase(Locale.ROOT);
            File target = new File(directory, UUID.randomUUID() + extension);
            status.setText(image ? "正在下载图片…" : "正在下载视频…");
            MaiChatAttachmentDownload.Scope scope = downloadScope;
            downloads.download(url, target, info.maxBytes,
                scope, downloaded -> activity.runOnUiThread(() -> {
                    if (!isShowing() || scope != downloadScope ||
                        !account.equals(MaiChatAuthClient.shared(activity).accountId())) {
                        target.delete();
                        return;
                    }
                    playing = false;
                    if (!downloaded.ok()) { status.setText("下载失败，请重试"); return; }
                    status.setText("");
                    if (!(image ? activity.openCloudDriveImage(target.getAbsolutePath()) :
                        activity.openCloudDriveVideo(target.getAbsolutePath()))) target.delete();
                }));
        });
    }

    private void cleanupIdlePlaybackCache() {
        MaiCloudDrivePlaybackCache.cleanupIdle(new File(activity.getCacheDir(),
            "cloud-drive-playback"));
    }

    private void delete(String id) {
        JSONObject payload = new JSONObject();
        try { payload.put("video_id", id); } catch (Exception ignored) { }
        status.setText("正在删除…");
        client.request(MaiCloudDrive.DELETE, payload, result -> {
            if (!result.ok()) { status.setText(MaiCloudDrive.errorText(result)); return; }
            status.setText(result.json().optBoolean("cleanup_pending") ?
                "已从列表移除，云端文件正在清理" : "已删除");
            refresh();
        });
    }

    private boolean live(int expected) {
        return isShowing() && expected == generation &&
            account.equals(MaiChatAuthClient.shared(activity).accountId());
    }

    private LinearLayout card() {
        LinearLayout card = new LinearLayout(activity);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setPadding(dp(14), dp(12), dp(14), dp(12));
        card.setBackground(MaiChatTheme.bordered(Color.WHITE, MaiChatTheme.BORDER, 12, activity));
        return card;
    }

    private TextView sectionTitle(String text) {
        TextView heading = label(text, 16, MaiChatTheme.TEXT);
        heading.setTypeface(MaiChatTypography.semibold());
        heading.setPadding(0, dp(16), 0, dp(10));
        return heading;
    }

    private TextView label(String text, int sp, int color) {
        TextView label = new TextView(activity);
        label.setText(text);
        label.setTextSize(sp);
        label.setTextColor(color);
        return label;
    }

    private Button button(String title, Runnable action) {
        Button button = new Button(activity);
        button.setText(title);
        button.setAllCaps(false);
        button.setTextSize(14);
        button.setMinHeight(0);
        button.setMinimumHeight(0);
        button.setElevation(0);
        button.setStateListAnimator(null);
        boolean primary = title.equals("上传图片或视频") || title.equals("继续上传");
        boolean destructive = title.equals("删除") || title.equals("放弃");
        button.setTextColor(primary ? Color.WHITE :
            destructive ? MaiChatTheme.RED : MaiChatTheme.BLUE_DARK);
        button.setBackground(primary ? MaiChatTheme.rounded(MaiChatTheme.BLUE, 10, activity) :
            MaiChatTheme.bordered(destructive ? Color.WHITE : MaiChatTheme.BLUE_SOFT,
                MaiChatTheme.BORDER, 10, activity));
        button.setOnClickListener(ignored -> action.run());
        return button;
    }

    private LinearLayout.LayoutParams row() {
        LinearLayout.LayoutParams layout = new LinearLayout.LayoutParams(-1, -2);
        layout.bottomMargin = dp(8);
        return layout;
    }

    private int dp(int value) {
        return (int) (value * activity.getResources().getDisplayMetrics().density + .5f);
    }

    private static String sizeText(long bytes) {
        if (bytes >= 1_000_000_000) return String.format(Locale.ROOT, "%.2f GB", bytes / 1e9);
        if (bytes >= 1_000_000) return String.format(Locale.ROOT, "%.1f MB", bytes / 1e6);
        if (bytes >= 1_000) return String.format(Locale.ROOT, "%.0f KB", bytes / 1e3);
        return Math.max(0, bytes) + " B";
    }
}
